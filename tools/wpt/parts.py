"""Which web-platform-tests the Runtime is held to, and which of M8's tickets each one belongs to.

darionxt/mitto#260 chose them: the directories that cover each part of M8, and the rest of that ticket's list, whole.
"""

from dataclasses import dataclass

__all__ = ["DIRECTORIES", "PARTS", "Part", "directory_of", "part_of"]


@dataclass(frozen=True, slots=True)
class Part:
    """A part of M8, with the tests that cover it.

    Attributes:
        ticket: The ticket that makes the part, such as "#267".
        title: What the part is, such as "Tree construction".
        include: Where its tests are: directories, which end in "/", or the start of their names.
        exclude: What of those belongs to another part, in the same form.
    """

    ticket: str
    title: str
    include: tuple[str, ...]
    exclude: tuple[str, ...] = ()

    def holds(self, test: str) -> bool:
        """Whether a test, by its path from the root of web-platform-tests, belongs to this part."""
        return test.startswith(self.include) and not test.startswith(self.exclude)


PARTS = (
    Part(
        "#267",
        "Tree construction",
        (
            "html/syntax/parsing/",
            "html/syntax/parsing-html-fragments/",
            "html/syntax/serializing-html-fragments/",
            "domparsing/",
        ),
    ),
    Part("#268", "Encodings", ("encoding/", "html/syntax/charset/", "encoding-detection/"), ("encoding/streams/",)),
    Part(
        "#269",
        "XML",
        (
            "html/the-xhtml-syntax/",
            "html/syntax/parsing-xml-fragments/",
            "html/syntax/serializing-xml-fragments/",
            "html/syntax/xmldecl/",
        ),
    ),
    Part(
        "#270",
        "Scripts",
        (
            "html/semantics/scripting-1/",
            "html/webappapis/scripting/",
            "html/webappapis/microtask-queuing/",
            "html/webappapis/timers/",
            "html/webappapis/atob/",
            "html/scripting/",
        ),
        (
            "html/semantics/scripting-1/the-script-element/module/",
            "html/semantics/scripting-1/the-script-element/json-module/",
            "html/semantics/scripting-1/the-script-element/css-module/",
        ),
    ),
    Part("#271", "document.write()", ("html/webappapis/dynamic-markup-insertion/",)),
    Part(
        "#272",
        "Modules",
        (
            "html/semantics/scripting-1/the-script-element/module/",
            "html/semantics/scripting-1/the-script-element/json-module/",
            "import-maps/",
        ),
    ),
    Part(
        "#273",
        "Events and observers",
        ("dom/events/", "dom/abort/", "dom/nodes/MutationObserver-", "html/webappapis/events/"),
    ),
    Part(
        "#274",
        "Element interfaces",
        (
            "dom/nodes/",
            "dom/lists/",
            "html/dom/",
            "html/semantics/grouping-content/",
            "html/semantics/text-level-semantics/",
            "html/semantics/tabular-data/",
            "html/semantics/sections/",
            "html/semantics/links/",
            "html/semantics/edits/",
            "html/semantics/interactive-elements/",
            "html/semantics/the-button-element/",
            "html/semantics/disabled-elements/",
            "html/infrastructure/common-dom-interfaces/",
            "html/infrastructure/urls/",
        ),
        ("dom/nodes/MutationObserver-", "html/dom/render-blocking/", "html/dom/partial-updates/"),
    ),
    Part(
        "#275",
        "Querying and walking the tree",
        ("dom/traversal/", "dom/ranges/", "dom/collections/", "html/semantics/selectors/"),
    ),
    Part("#276", "Shadow trees and custom elements", ("shadow-dom/", "custom-elements/")),
    Part(
        "#277",
        "Structured clone and messaging",
        (
            "webmessaging/",
            "html/webappapis/structured-clone/",
            "html/infrastructure/safe-passing-of-structured-data/",
        ),
    ),
    Part("#278", "requestAnimationFrame()", ("html/webappapis/animation-frames/",)),
    Part(
        "#279",
        "Navigations",
        ("html/browsers/browsing-the-web/", "html/semantics/document-metadata/the-meta-element/"),
    ),
    Part("#280", "Session history", ("html/browsers/history/",)),
    Part("#281", "Forms", ("html/semantics/forms/",)),
    Part(
        "#283",
        "XMLHttpRequest and fetch()",
        (
            "xhr/",
            "fetch/api/",
            "fetch/data-urls/",
            "fetch/content-type/",
            "fetch/nosniff/",
            "fetch/redirects/",
            "fetch/range/",
            "fetch/h1-parsing/",
            "FileAPI/",
        ),
    ),
    Part(
        "#284",
        "Subresources",
        (
            "html/semantics/document-metadata/",
            "html/syntax/speculative-parsing/",
            "html/semantics/embedded-content/the-img-element/",
        ),
        ("html/semantics/document-metadata/the-meta-element/",),
    ),
    Part(
        "#285",
        "Frames",
        (
            "html/semantics/embedded-content/the-iframe-element/",
            "html/browsers/windows/",
            "html/browsers/the-window-object/",
        ),
    ),
    Part("#286", "Access between frames", ("html/browsers/origin/", "html/browsers/the-windowproxy-exotic-object/")),
    Part("#287", "Web Storage", ("webstorage/",)),
    Part("#288", "IndexedDB and the Cache API", ("IndexedDB/", "service-workers/cache-storage/")),
    Part("#289", "Dedicated workers", ("workers/",)),
)
"""The parts of M8 that web-platform-tests cover. A test belongs to at most one."""

_TICKET_LIST = (
    "html/syntax/parsing/",
    "html/semantics/scripting-1/",
    "html/webappapis/",
    "html/browsers/",
    "html/semantics/forms/",
    "dom/",
    "domparsing/",
    "custom-elements/",
    "shadow-dom/",
    "webmessaging/",
    "webstorage/",
    "IndexedDB/",
    "xhr/",
    "fetch/",
    "FileAPI/",
    "encoding/",
    "workers/",
)
"""The directories that #260's ticket listed, which the copy takes whole."""


def _directories() -> tuple[str, ...]:
    named = {prefix for part in PARTS for prefix in part.include if prefix.endswith("/")} | set(_TICKET_LIST)
    # A directory inside another that the copy takes whole adds nothing to the copy.
    return tuple(sorted(d for d in named if not any(d != other and d.startswith(other) for other in named)))


DIRECTORIES = _directories()
"""The directories that the copy takes whole, each ending in "/"."""

_PLACES = tuple(
    sorted(
        {*_TICKET_LIST, *(prefix for part in PARTS for prefix in (*part.include, *part.exclude))},
        key=len,
        reverse=True,
    )
)


def part_of(test: str) -> Part | None:
    """The part of M8 that a test, by its path from the root of web-platform-tests, belongs to, if any."""
    return next((part for part in PARTS if part.holds(test)), None)


def directory_of(test: str) -> str:
    """Where a test is, as the table of #260 counts it: the longest directory or start of a name that a part or the
    ticket's list names and that holds the test, such as "dom/nodes/MutationObserver-" or "fetch/".

    Raises:
        ValueError: No such place holds the test.
    """
    for place in _PLACES:
        if test.startswith(place):
            return place
    raise ValueError(f"{test} is in none of the directories of web-platform-tests that the Runtime is held to")
