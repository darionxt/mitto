"""How long a request may take."""

from dataclasses import dataclass

__all__ = ["NO_TIMEOUT", "Timeout", "to_timeout"]

# About 23 days. The native core takes a timer as 32 bits of milliseconds.
_MAX_SECONDS = 2_000_000


@dataclass(frozen=True, slots=True, kw_only=True)
class Timeout:
    """A request's timers, in seconds, which the native core runs. None runs no timer, leaving what //net has of its own.

    A timer that runs out raises a RequestTimeoutError, and the Session carries on.

    Attributes:
        connect: The time from the start of the request, or from a redirect, until a connection is ready for it: the name
            is resolved, a connection is free and any proxy tunnel and TLS handshake are done. In a Session with a cache, this
            also counts a wait for another request's response to the same URL, and a response from the cache stops it.
        read: The longest wait for bytes from the server. The response's headers count as one wait, since //net does not
            report them a byte at a time, and it does not run out while the request's body is still moving. A wait for the
            next part of a stream body is a wait for the caller, and does not count.
        total: The time from the start of the request to its end, over all its redirects.

    Raises:
        ValueError: If a value is not a positive number of at most 2,000,000 seconds.
    """

    connect: float | None = None
    read: float | None = None
    total: float | None = None

    def __post_init__(self) -> None:
        for name, seconds in (("connect", self.connect), ("read", self.read), ("total", self.total)):
            # NaN fails this as well.
            if seconds is not None and not 0 < seconds <= _MAX_SECONDS:
                raise ValueError(f"{name} has to be more than 0 and at most {_MAX_SECONDS} seconds, not {seconds}")


NO_TIMEOUT = Timeout()


def to_timeout(timeout: float | Timeout | None) -> Timeout:
    """Return the timers that a caller's `timeout` argument stands for: a number is the total time, and None is no timer.

    Raises:
        ValueError: As Timeout does.
    """
    if timeout is None:
        return NO_TIMEOUT
    return timeout if isinstance(timeout, Timeout) else Timeout(total=timeout)
