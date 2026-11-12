"""What a page's navigator reads of its browser: the user agent, the user agent client hints as data, and the languages.

Chrome's browser hands each renderer one UserAgentMetadata, from which it writes the user agent client hints and from
which navigator.userAgentData gives a page the same values. A Persona holds those hints as its Chrome sent them, so this
reads the Persona's own lines back into that data, and a page and the Session's requests cannot disagree. See
docs/persona.md, "The Runtime".
"""

# Upstream: third_party/blink/public/common/user_agent/user_agent_metadata.h
# Upstream: third_party/blink/renderer/core/frame/navigator_ua.cc
# Upstream: third_party/blink/renderer/core/execution_context/navigator_base.cc

from collections.abc import Mapping

from mitto._machine import Machine
from mitto._native import BrandVersion, PageNavigator, PageUserAgentData
from mitto._persona import Persona, RequestKind

__all__ = ["accept_languages", "page_navigator", "user_agent_data"]

# What navigator.platform gives for each operating system that a Persona claims, as GetReducedNavigatorPlatform() in
# navigator_base.cc writes it into Chrome for that system.
_NAVIGATOR_PLATFORMS: Mapping[str, str] = {"Windows": "Win32"}

# The lines of a navigation hold every user agent client hint that a Persona's Chrome sends.
_HINTS_KIND = RequestKind.NAVIGATION


def page_navigator(persona: Persona, accept_language: str, machine: Machine) -> PageNavigator:
    """Return what a Page's navigator reads of a Session with this Persona, Accept-Language and Machine.

    Raises:
        ValueError: If the Persona claims an operating system that no navigator.platform is known for, or lacks a user
            agent client hint.
    """
    platform = _NAVIGATOR_PLATFORMS.get(persona.operating_system)
    if platform is None:
        raise ValueError(f"no navigator.platform is known for a Persona of {persona.operating_system!r}")
    return PageNavigator(
        user_agent=persona.user_agent,
        platform=platform,
        languages=accept_languages(accept_language),
        user_agent_data=user_agent_data(persona, machine),
    )


def accept_languages(accept_language: str) -> str:
    """Return the languages of Chrome's settings that give an Accept-Language, separated by commas.

    Chrome writes Accept-Language from its language setting, adding a weight to each language after the first, and a
    page's navigator.languages comes from the setting itself. So the setting is the header's languages in their order,
    without their weights: "en-US,en" for "en-US,en;q=0.9". An empty header gives no languages.
    """
    tags = (item.split(";", 1)[0].strip() for item in accept_language.split(","))
    return ",".join(tag for tag in tags if tag)


def user_agent_data(persona: Persona, machine: Machine) -> PageUserAgentData:
    """Return the UserAgentMetadata that gave the Persona's user agent client hints, with the Machine's platform version.

    Raises:
        ValueError: If the Persona lacks one of the hints, or one does not hold what its kind of value is.
    """
    lines = {name.lower(): value for name, value in persona.request_defaults(_HINTS_KIND)["headers"]}

    def hint(name: str) -> str:
        if name not in lines:
            raise ValueError(f"the Persona's {_HINTS_KIND.value} lines have no {name}")
        return lines[name]

    return PageUserAgentData(
        brands=_brand_list(hint("sec-ch-ua")),
        full_version_list=_brand_list(hint("sec-ch-ua-full-version-list")),
        mobile=_boolean(hint("sec-ch-ua-mobile")),
        platform=_string(hint("sec-ch-ua-platform")),
        platform_version=machine.platform_version,
        architecture=_string(hint("sec-ch-ua-arch")),
        bitness=_string(hint("sec-ch-ua-bitness")),
        model=_string(hint("sec-ch-ua-model")),
        ua_full_version=_string(hint("sec-ch-ua-full-version")),
        wow64=_boolean(hint("sec-ch-ua-wow64")),
        form_factors=tuple(_string(item) for item in _list_items(hint("sec-ch-ua-form-factors"))),
    )


# The values of the user agent client hints are structured fields (RFC 9651), which Chrome writes in
# third_party/blink/common/user_agent/user_agent_metadata.cc and content/browser/client_hints/client_hints.cc: a string, a
# boolean, a list of strings, or a list of brands, each a string with its version as the parameter "v". These read back
# what those write.
# Upstream: third_party/blink/common/user_agent/user_agent_metadata.cc
# Upstream: content/browser/client_hints/client_hints.cc


def _brand_list(value: str) -> tuple[BrandVersion, ...]:
    brands: list[BrandVersion] = []
    for item in _list_items(value):
        brand, separator, parameter = item.partition(";")
        name, equals, version = parameter.partition("=")
        if not separator or not equals or name.strip() != "v":
            raise ValueError(f"{item!r} is not a brand with its version")
        brands.append(BrandVersion(_string(brand), _string(version)))
    return tuple(brands)


def _list_items(value: str) -> list[str]:
    """Split a list into its items, at the commas outside its strings."""
    items: list[str] = []
    start = 0
    quoted = escaped = False
    for index, character in enumerate(value):
        if escaped:
            escaped = False
        elif character == "\\" and quoted:
            escaped = True
        elif character == '"':
            quoted = not quoted
        elif character == "," and not quoted:
            items.append(value[start:index].strip())
            start = index + 1
    items.append(value[start:].strip())
    return [item for item in items if item]


def _string(value: str) -> str:
    text = value.strip()
    if len(text) < 2 or text[0] != '"' or text[-1] != '"':
        raise ValueError(f"{value!r} is not a string")
    inner = text[1:-1]
    unescaped: list[str] = []
    escaped = False
    for character in inner:
        if escaped:
            if character not in '"\\':
                raise ValueError(f"{value!r} escapes {character!r}")
            unescaped.append(character)
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == '"':
            raise ValueError(f"{value!r} is not one string")
        else:
            unescaped.append(character)
    if escaped:
        raise ValueError(f"{value!r} ends in an escape")
    return "".join(unescaped)


def _boolean(value: str) -> bool:
    text = value.strip()
    if text not in ("?0", "?1"):
        raise ValueError(f"{value!r} is not a boolean")
    return text == "?1"
