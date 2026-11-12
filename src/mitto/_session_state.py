"""What a Session holds from when it opens until it closes, whichever API drives it.

Session and AsyncSession differ in how they wait, so everything that has nothing to do with waiting is here. Opening and
closing can block for a moment, on the system's DNS settings, a cache on disk or a file of cookies.
"""

import os
from collections.abc import Buffer, Callable, Iterable, Sequence
from pathlib import Path
from typing import cast

from mitto import _native
from mitto._arguments import require_path, require_type
from mitto._cookie_file import CookieFile, read_cookie_file, write_cookie_file
from mitto._cookies import Cookies
from mitto._machine import Machine, draw_noise_salts
from mitto._native import MEMORY_CACHE, NativePage, NetworkQuality, PageRequestTemplate, PageStart, PrivateSettings
from mitto._persona import RequestKind, load_persona
from mitto._request import BuiltRequest, RequestOptions, build_request
from mitto._timeout import Timeout, to_timeout

__all__ = ["ABOUT_BLANK", "SessionState"]

ABOUT_BLANK = "about:blank"
"""The URL of a Page that navigates nowhere, as a new tab's first document needs no request."""

# The Request Kinds of what a Page asks its Session for: its navigation, its classic scripts and its scripts' fetch().
_PAGE_REQUEST_KINDS = (RequestKind.NAVIGATION, RequestKind.SCRIPT, RequestKind.FETCH)
# The template of a Page's fetch() with a body, beside one per Request Kind.
_FETCH_WITH_BODY = "fetch_with_body"


class SessionState:
    """A native Session with its Persona, its Machine, its default timeout and the file of its cookies.

    Attributes:
        machine: The Machine it claims, whose values its Persona's lines hold, and which its file of cookies keeps.
        native: The native Session.
        cookies: Its cookies.
    """

    def __init__(
        self,
        *,
        persona: str,
        machine: Machine | None,
        accept_language: str | None,
        additional_trust_anchors: Sequence[str],
        timeout: float | Timeout | None,
        cookie_file: str | os.PathLike[str] | None,
        proxy: str | None,
        cache: str | os.PathLike[str] | None,
        settings: PrivateSettings | None,
    ) -> None:
        """Open a native Session and load any cookies, where Session() says what each argument means.

        Raises:
            TypeError: As Session() does.
            ValueError: As Session() does.
            OSError: As Session() does.
            NativeLibraryError: If the native library cannot be used.
        """
        # Before the library, so that these faults are named whether or not there is one.
        require_type("persona", persona, str, "text")
        require_type("machine", machine, Machine | None, "a Machine or None")
        require_type("accept_language", accept_language, str | None, "text or None")
        anchors = _trust_anchors(additional_trust_anchors)
        require_path("cookie_file", cookie_file, or_none=True)
        require_type("proxy", proxy, str | None, "text or None")
        require_path("cache", cache, or_none=True)
        persona_data = load_persona(persona)
        self._timeout = to_timeout(timeout)
        # Before anything that needs closing exists, because this can raise. The working directory may change before the
        # Session closes.
        self._cookie_file = None if cookie_file is None else Path(cookie_file).resolve()
        saved = None if self._cookie_file is None else _read_if_there(self._cookie_file)
        self.machine = _machine_to_claim(machine, saved)
        self._persona = persona_data.with_machine(self.machine)
        # Each time the Session opens, for the Machine it claims, which may be the file's.
        salts = draw_noise_salts(self.machine)
        self.native = _native.Session(
            accept_language=self._persona.accept_language if accept_language is None else accept_language,
            user_agent=self._persona.user_agent,
            additional_trust_anchors=anchors,
            proxy=proxy,
            http_cache=_http_cache_setting(cache),
            network_quality=NetworkQuality(
                http_rtt_ms=self.machine.round_trip_ms,
                downstream_throughput_kbps=self.machine.throughput_kbps,
                browser_salt=salts.browser,
                renderer_salt_key=salts.renderer_key,
            ),
            **(PrivateSettings() if settings is None else settings),
        )
        self.cookies = Cookies(self.native, self.machine)
        if saved is not None:
            self._add_saved(saved)

    def build(self, method: str, url: str, options: RequestOptions) -> BuiltRequest:
        """Return what to ask the native Session for, with this Session's default timeout unless `options` has its own.

        Raises:
            TypeError: As Session.request() does.
            ValueError: As Session.request() does.
        """
        options.setdefault("timeout", self._timeout)
        return build_request(self._persona, method, url, **options)

    def open_page(self, url: str) -> NativePage:
        """Open a Page that navigates to `url` through this Session, or nowhere for ABOUT_BLANK.

        It starts with this Session's Machine, its time zone, processors, window and the rest of what a page reads, and the
        locale of its Persona. Its requests send what this Session's Persona says each Request Kind sends, as a request of
        that kind does, and run no timers, as Chrome's have none, whatever this Session's default timeout says.

        Raises:
            ValueError: If `url` holds a NUL character or cannot be encoded as UTF-8, or the native Session has closed.
            NativeLibraryError: If this build of mitto has no Runtime.
            OSError: If the Page's process cannot start.
        """
        start = self._page_start()
        if url == ABOUT_BLANK:
            return self.native.open_page(start)
        requests = {kind.value: self._page_request_template(kind) for kind in _PAGE_REQUEST_KINDS}
        # A fetch() with a body sends the lines that one has, with its Content-Type where the Persona puts it.
        requests[_FETCH_WITH_BODY] = self._page_request_template(RequestKind.FETCH, with_body=True)
        return self.native.open_page(start, url=url, requests=requests)

    def _page_start(self) -> PageStart:
        """Return what a Page starts with: what this Session's Machine and Persona say of what a page reads."""
        machine = self.machine
        return PageStart(
            time_zone=machine.time_zone,
            locale=self._persona.locale,
            processors=machine.processors,
            memory=machine.memory,
            touch_points=machine.touch_points,
            work_area_x=machine.work_area_x,
            work_area_y=machine.work_area_y,
            work_area_width=machine.work_area_width,
            work_area_height=machine.work_area_height,
            window_state=machine.window_state,
            window_x=machine.window_x,
            window_y=machine.window_y,
            window_width=machine.window_width,
            window_height=machine.window_height,
            contrast_theme=machine.contrast_theme,
            inverted_colors=machine.inverted_colors,
        )

    def _page_request_template(self, kind: RequestKind, *, with_body: bool = False) -> PageRequestTemplate:
        """Return what a Page's request of `kind` sends, as this Session's Persona says."""
        defaults = self._persona.request_defaults(kind, with_body=with_body)
        return PageRequestTemplate(
            headers=defaults["headers"],
            priority=defaults["priority"],
            priority_incremental=defaults["priority_incremental"],
            fetch=defaults["fetch"],
        )

    def begin_close(self) -> Callable[[], None]:
        """Take the cookies to save and refuse every later call to the native Session, none of which blocks for long.

        Returns:
            What does the rest, which is the part that can block: it writes the cookies to the file and closes the native
            Session, which it does even if the write fails. It raises an OSError if the file cannot be written. What a
            later call returns does nothing.
        """
        cookie_file, self._cookie_file = self._cookie_file, None
        to_save = None if cookie_file is None else (cookie_file, self.machine, self.native.list_cookies())
        destroy = self.native.detach()

        def finish() -> None:
            try:
                if to_save is not None:
                    write_cookie_file(*to_save)
            finally:
                destroy()

        return finish

    def close(self) -> None:
        """Save the cookies and close the native Session, as begin_close() and what it returns do.

        Raises:
            OSError: If the file of cookies cannot be written.
        """
        self.begin_close()()

    def _add_saved(self, saved: CookieFile) -> None:
        """Add the cookies of the file that this Session claims the Machine of. Close the Session if that fails."""
        try:
            saved.add_to(self.native)
        except BaseException:
            self.native.close()
            raise


def _read_if_there(path: Path) -> CookieFile | None:
    """Return what a Session's file of cookies holds, or None for a file that does not exist yet.

    Raises:
        OSError: If it exists and cannot be read.
        ValueError: If it holds no cookies that a Session can use.
    """
    try:
        return read_cookie_file(path)
    except FileNotFoundError:
        return None


def _machine_to_claim(machine: Machine | None, saved: CookieFile | None) -> Machine:
    """Return the Machine of a Session's file of cookies, or else the one given, or else one drawn for it.

    A Session that comes back with its cookies has to be the machine it was, since a server can tell.

    Raises:
        ValueError: If a Machine is given and the file holds another.
    """
    if saved is None:
        return Machine.draw() if machine is None else machine
    if machine is None:
        return saved.machine
    if machine != saved.machine:
        raise ValueError(
            f"{saved.path} holds the cookies of another Machine than `machine` gives: leave it out to claim the file's"
        )
    return machine


def _trust_anchors(anchors: object) -> tuple[str, ...]:
    """Return what Session() takes as `additional_trust_anchors`, each the text of a PEM file.

    Raises:
        TypeError: If it is text or bytes, whose every character would be an anchor of its own, or anything else that is
            not an iterable, or an anchor is not text.
    """
    if isinstance(anchors, str | Buffer) or not isinstance(anchors, Iterable):
        raise TypeError(f"additional_trust_anchors has to be a sequence of PEM texts, not {type(anchors).__name__}")
    # A generator goes once, so its anchors are kept for the native Session.
    texts = tuple(cast(Iterable[object], anchors))
    for anchor in texts:
        require_type("an item of additional_trust_anchors", anchor, str, "text")
    return cast(tuple[str, ...], texts)


def _http_cache_setting(cache: str | os.PathLike[str] | None) -> str | None:
    """Return the native Session's setting for what Session() takes as `cache`.

    Raises:
        ValueError: If `cache` is an empty string, which would otherwise mean the working directory.
    """
    if cache is None:
        return None
    if cache == "":
        raise ValueError("cache is an empty string, which names no directory")
    if cache == MEMORY_CACHE:
        return MEMORY_CACHE
    # The working directory may change before the Session closes, and the native core only takes an absolute path.
    return str(Path(cache).resolve())
