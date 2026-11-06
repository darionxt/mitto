"""What tests need to know of client hints: which of a Persona's lines go only once a server has asked."""

from collections.abc import Iterable

from tools.capture.normal_form import HINTS_FROM_THE_MACHINE

SENT_BY_DEFAULT = frozenset({"sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform"})
"""The client hints that go whether or not a server has asked."""

HINTS_A_SERVER_ASKS_FOR = frozenset(
    {
        *HINTS_FROM_THE_MACHINE,
        "sec-ch-ua-arch",
        "sec-ch-ua-bitness",
        "sec-ch-ua-form-factors",
        "sec-ch-ua-full-version",
        "sec-ch-ua-full-version-list",
        "sec-ch-ua-model",
        "sec-ch-ua-platform-version",
        "sec-ch-ua-wow64",
    }
)
"""The rest of what a Persona has lines for, which the native core removes until a server asks."""


def until_a_server_asks(lines: Iterable[tuple[str, str]]) -> list[tuple[str, str]]:
    """Return what the native core leaves of a Persona's (name, value) lines for an origin that has asked for nothing."""
    return [line for line in lines if line[0].lower() not in HINTS_A_SERVER_ASKS_FOR]
