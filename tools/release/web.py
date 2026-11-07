"""Asking a web service, with what can stand in for the network in tests."""

import json
import urllib.request
from collections.abc import Callable
from typing import Any, cast

__all__ = ["Fetch", "WebError", "fetch_from_the_network", "json_answer", "version_parts"]

_TIMEOUT_SECONDS = 30.0

type Fetch = Callable[[str], bytes]
"""What returns the body of the answer to a URL, so that tests can give saved answers."""


class WebError(Exception):
    """A web service did not answer, or its answer is not what the release routine reads."""


def fetch_from_the_network(url: str) -> bytes:
    """Return the body of the answer to a URL.

    Raises:
        WebError: The request failed.
    """
    try:
        with urllib.request.urlopen(url, timeout=_TIMEOUT_SECONDS) as answer:
            return cast(bytes, answer.read())
    except OSError as error:
        raise WebError(f"{url} did not answer: {error}") from error


def json_answer(body: bytes, url: str) -> Any:
    """Return the JSON in the body of an answer.

    Raises:
        WebError: The body is not JSON.
    """
    try:
        return json.loads(body)
    except ValueError as error:
        raise WebError(f"{url} did not give JSON: {error}") from error


def version_parts(version: str, url: str) -> tuple[int, ...]:
    """Return the four numbers of a Chrome version, such as (157, 0, 8103, 39), so that versions compare as numbers.

    Raises:
        WebError: The version is not four numbers.
    """
    parts = version.split(".")
    if len(parts) != 4 or not all(part.isdigit() for part in parts):
        raise WebError(f"{url} gave a version that is not four numbers: {version}")
    return tuple(int(part) for part in parts)
