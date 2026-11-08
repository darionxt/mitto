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
from mitto._cookies import Cookies, write_cookie_file
from mitto._machine import Machine, draw_noise_salts
from mitto._native import MEMORY_CACHE, NetworkQuality, PrivateSettings
from mitto._persona import load_persona
from mitto._request import BuiltRequest, RequestOptions, build_request
from mitto._timeout import Timeout, to_timeout

__all__ = ["SessionState"]


class SessionState:
    """A native Session with its Persona, its Machine, its default timeout and the file of its cookies.

    Attributes:
        machine: The Machine it claims, whose values its Persona's lines hold.
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
        self.machine = Machine.draw() if machine is None else machine
        self._persona = load_persona(persona).with_machine(self.machine)
        salts = draw_noise_salts(self.machine)
        self._timeout = to_timeout(timeout)
        # Before anything that needs closing exists, because this can raise. The working directory may change before the
        # Session closes.
        self._cookie_file = None if cookie_file is None else Path(cookie_file).resolve()
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
        self.cookies = Cookies(self.native)
        if self._cookie_file is not None:
            self._load_cookie_file(self._cookie_file)

    def build(self, method: str, url: str, options: RequestOptions) -> BuiltRequest:
        """Return what to ask the native Session for, with this Session's default timeout unless `options` has its own.

        Raises:
            TypeError: As Session.request() does.
            ValueError: As Session.request() does.
        """
        options.setdefault("timeout", self._timeout)
        return build_request(self._persona, method, url, **options)

    def begin_close(self) -> Callable[[], None]:
        """Take the cookies to save and refuse every later call to the native Session, none of which blocks for long.

        Returns:
            What does the rest, which is the part that can block: it writes the cookies to the file and closes the native
            Session, which it does even if the write fails. It raises an OSError if the file cannot be written. What a
            later call returns does nothing.
        """
        cookie_file, self._cookie_file = self._cookie_file, None
        to_save = None if cookie_file is None else (cookie_file, self.native.list_cookies())
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

    def _load_cookie_file(self, path: Path) -> None:
        """Load what `path` holds, where a file that does not exist yet holds nothing. Close the Session if that fails."""
        try:
            self.cookies.load(path)
        except FileNotFoundError:
            pass
        except BaseException:
            self.native.close()
            raise


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
