"""Bounds-checked reading of the binary structures the capture parsers share."""


class ParseError(ValueError):
    """The input is not a valid instance of what was being parsed."""


class Reader:
    """A cursor over bytes. Reading past the end raises ParseError instead of returning less."""

    __slots__ = ("_data", "_position")

    def __init__(self, data: bytes) -> None:
        self._data = data
        self._position = 0

    @property
    def empty(self) -> bool:
        return self._position == len(self._data)

    def take(self, count: int) -> bytes:
        end = self._position + count
        if end > len(self._data):
            raise ParseError(f"wanted {count} bytes, {len(self._data) - self._position} left")
        taken = self._data[self._position : end]
        self._position = end
        return taken

    def rest(self) -> bytes:
        """Read all the input that is left."""
        return self.take(len(self._data) - self._position)

    def uint(self, size: int) -> int:
        """Read a big-endian unsigned integer of size bytes."""
        return int.from_bytes(self.take(size))

    def vector(self, length_size: int) -> "Reader":
        """Read a length-prefixed vector, whose length takes length_size bytes, as its own Reader."""
        return Reader(self.opaque(length_size))

    def opaque(self, length_size: int) -> bytes:
        """Read a length-prefixed vector, whose length takes length_size bytes, as bytes."""
        return self.take(self.uint(length_size))

    def uints(self, size: int) -> tuple[int, ...]:
        """Read the rest of the input as integers of size bytes each."""
        values: list[int] = []
        while not self.empty:
            values.append(self.uint(size))
        return tuple(values)

    def finish(self) -> None:
        """Raise ParseError if any input is left."""
        if not self.empty:
            raise ParseError(f"{len(self._data) - self._position} unexpected bytes at the end")
