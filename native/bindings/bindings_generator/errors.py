"""What the generator raises when the database holds something it does not handle."""

__all__ = ["UnhandledError"]


class UnhandledError(Exception):
    """The database holds a construct, an extended attribute or a type that the generator does not handle.

    The message names it and where it is, so that a new Engine Release whose Web IDL files bring something new stops
    the build with it, rather than getting bindings that are wrong.
    """
