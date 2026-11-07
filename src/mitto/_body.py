"""Encoding a request's body as Chrome does.

A mapping follows URLSearchParams, and a multipart body follows Blink's FormDataEncoder. What a body adds to a request's
headers comes from the Persona, as docs/persona.md says. Only a stream does I/O, on a caller's file or iterable: a file says
its size when the body is made, and the rest waits until something iterates it.
"""

import io
import json
import os
import re
import secrets
from collections.abc import AsyncIterable, AsyncIterator, Buffer, Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from enum import StrEnum
from typing import BinaryIO, Protocol, cast, runtime_checkable
from urllib.parse import quote_plus

__all__ = [
    "BodyEncoding",
    "EncodedBody",
    "FileFields",
    "FilePart",
    "QueryParams",
    "ReadableFile",
    "RequestData",
    "StreamBody",
    "async_stream_chunks",
    "encode_body",
    "encode_query",
    "with_declared_size",
]

type QueryParams = Mapping[str, str] | Iterable[tuple[str, str]]
"""A query as names and values. Pairs can repeat a name."""


@runtime_checkable
class ReadableFile(Protocol):
    """A file object opened in binary mode, which is all a stream asks of one. It may also seek, to tell its size."""

    def read(self, size: int, /) -> Buffer: ...


type RequestData = Buffer | str | Mapping[str, str] | ReadableFile | Iterable[Buffer]
"""A body as bytes, as text, as form fields by name, or as a stream from a file object or an iterable of bytes."""

type FilePart = bytes | tuple[str, bytes] | tuple[str, bytes, str]
"""A file's content, or (file name, content), or (file name, content, media type)."""

type FileFields = Mapping[str, FilePart] | Iterable[tuple[str, FilePart]]
"""The files of a multipart body by field name. Pairs can repeat a name."""

# What Blink appends to the boundary's prefix, indexed by six random bits. "A" and "B" come twice, as they do there.
_BOUNDARY_PREFIX = "----WebKitFormBoundary"
_BOUNDARY_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789AB"
_BOUNDARY_RANDOM_CHARACTERS = 16
_LOW_SIX_BITS = 0x3F

# What Blink calls a Blob that has no file name when it goes into a FormData, and a file that has no type.
_BLOB_FILE_NAME = "blob"
_NO_MEDIA_TYPE = "application/octet-stream"

# How much of a file a stream reads at a time: as much as the native core holds.
_FILE_READ_SIZE = 64 * 1024

_LINE_BREAK = re.compile(r"\r\n|\r|\n")
_LONE_SURROGATE = re.compile("[\ud800-\udfff]")
_MEDIA_TYPE = re.compile(r"[\x20-\x7e]+")


class BodyEncoding(StrEnum):
    """What a body's bytes are. With the Request Kind, this decides its Content-Type. The values are keys of a Persona."""

    TEXT = "text"
    FORM = "form"
    JSON = "json"
    MULTIPART = "multipart"


@dataclass(frozen=True, slots=True)
class EncodedBody:
    """A request's body, ready to send.

    Attributes:
        content: The bytes to send. They stay out of repr(), since they can be large.
        encoding: What they are, or None for bytes that a caller gave as they are, for which Chrome sends no Content-Type.
        boundary: What separates the parts of a multipart body, and otherwise nothing.
    """

    content: bytes = field(repr=False)
    encoding: BodyEncoding | None = None
    boundary: str = ""


@dataclass(frozen=True, slots=True)
class StreamBody:
    """A request's body that comes a part at a time, as the network stack asks for it. It goes only once.

    Attributes:
        chunks: What gives the bytes, which reads the caller's file or iterable as it goes.
        size: How many bytes it gives, or None if that is not known, which leaves out a Content-Length, and which Chrome
            refuses to send over HTTP/1.1.
    """

    chunks: Iterator[memoryview] = field(repr=False)
    size: int | None = None


def encode_body(
    data: RequestData | None, json_value: object, files: FileFields | None
) -> EncodedBody | StreamBody | None:
    """Return the body that a caller's arguments make, or None if they make none.

    Args:
        data: Bytes go as they are. Text goes as UTF-8. A mapping is encoded as URLSearchParams does, or with `files`, it
            holds the text fields of the multipart body. A file object goes from where it stands, and has a size if it can
            seek. An iterable gives parts of the body, and has no size.
        json_value: The value to write as JSON.stringify does, or None for none. So it writes no JSON null, which `data`
            can send.
        files: The files of a multipart body, which follow any text fields in `data`.

    Raises:
        TypeError: If `data` is of no type that makes a body, such as a file in text mode, a name or value in a mapping in
            it is not text, `files` is not a mapping or pairs with text for names, a file has another form, or `json_value`
            holds a value that JSON has no form of. The message names the argument. A stream raises it later, if a part it
            gives is not bytes.
        ValueError: If the arguments do not go together, because `json_value` is given beside another, or `data` is not a
            mapping beside `files`, or `json_value` holds a NaN or an infinity, which JSON cannot hold.
    """
    if json_value is not None:
        if data is not None or files is not None:
            raise ValueError("json cannot go together with data or files")
        return EncodedBody(_encode_json(json_value), BodyEncoding.JSON)
    if files is not None:
        return _encode_multipart(_fields_beside_files(data), files)
    match data:
        case None:
            return None
        case str():
            return EncodedBody(_well_formed(data).encode(), BodyEncoding.TEXT)
        case Mapping():
            return EncodedBody(_encode_form(cast(Mapping[str, str], data), "data").encode(), BodyEncoding.FORM)
        case Buffer():
            return EncodedBody(bytes(data))
        case ReadableFile():
            return _file_stream(data)
        case Iterable():
            return StreamBody(_byte_view(chunk) for chunk in data)
        case _:
            raise TypeError(f"data has to be bytes, text, a mapping, a file or an iterable, not {type(data).__name__}")


def with_declared_size(body: StreamBody, size: int) -> StreamBody:
    """Return `body` with the size that a caller gave for it, which the stream raises a ValueError if it does not keep."""
    return StreamBody(_exactly(body.chunks, size), size)


def _file_stream(file: ReadableFile) -> StreamBody:
    if isinstance(file, io.TextIOBase):
        raise TypeError("data has to be a file opened in binary mode, because a stream sends bytes")
    size = _size_left(cast(BinaryIO, file))
    return StreamBody(_file_chunks(file, size), size)


def _size_left(file: BinaryIO) -> int | None:
    """Return how many bytes `file` holds from where it stands, or None if it cannot say, as a pipe cannot."""
    try:
        position = file.tell()
        file.seek(0, os.SEEK_END)
        end = file.tell()
        file.seek(position)
    except (AttributeError, OSError):
        return None
    return max(end - position, 0)


def _file_chunks(file: ReadableFile, size: int | None) -> Iterator[memoryview]:
    """Yield what `file` holds, up to `size`. A file that has grown goes no further, as with a File in Chrome."""
    left = size
    while left != 0:
        chunk = _byte_view(file.read(_FILE_READ_SIZE if left is None else min(left, _FILE_READ_SIZE)))
        if not chunk.nbytes:
            return
        left = None if left is None else left - chunk.nbytes
        yield chunk


def _byte_view(chunk: object) -> memoryview:
    if not isinstance(chunk, Buffer):
        raise TypeError(f"a stream has to give bytes, not {type(chunk).__name__}")
    return memoryview(chunk).cast("B")


def _exactly(chunks: Iterator[memoryview], size: int) -> Iterator[memoryview]:
    total = 0
    for chunk in chunks:
        total += chunk.nbytes
        if total > size:
            raise ValueError(f"data gave more bytes than its Content-Length of {size}")
        yield chunk
    if total < size:
        raise ValueError(f"data gave {total} bytes, fewer than its Content-Length of {size}")


async def async_stream_chunks(data: AsyncIterable[object], size: int | None) -> AsyncIterator[memoryview]:
    """Yield what an async iterable gives, as a stream does what an iterable gives.

    Raises:
        TypeError: If a part it gives is not bytes.
        ValueError: If `size` is known, and it does not give that many bytes.
    """
    total = 0
    async for part in data:
        chunk = _byte_view(part)
        total += chunk.nbytes
        if size is not None and total > size:
            raise ValueError(f"data gave more bytes than its Content-Length of {size}")
        yield chunk
    if size is not None and total < size:
        raise ValueError(f"data gave {total} bytes, fewer than its Content-Length of {size}")


def encode_query(params: QueryParams) -> str:
    """Return `params` as the text of a query, as URLSearchParams.toString() does.

    Raises:
        TypeError: If `params` is not a mapping or pairs, or a name or value in it is not text.
    """
    return _encode_form(params, "params")


def _encode_form(fields: object, argument: str) -> str:
    """Return `fields`, which a caller gave as `argument`, as URLSearchParams.toString() writes them."""
    pairs = _text_fields(fields, argument)
    return "&".join(f"{_encode_component(name)}={_encode_component(value)}" for name, value in pairs)


def _text_fields(fields: object, argument: str) -> Iterator[tuple[str, str]]:
    """Yield the names and values of `fields`, which a caller gave as `argument`.

    Raises:
        TypeError: As _named_items() does, or if a value is not text. The message leaves out the value, which may be a
            secret.
    """
    for name, value in _named_items(fields, argument):
        if not isinstance(value, str):
            raise TypeError(f"the value of {name!r} in {argument} has to be text, not {type(value).__name__}")
        yield name, value


def _named_items(items: object, argument: str) -> Iterator[tuple[str, object]]:
    """Yield what a mapping or pairs hold by name, which a caller gave as `argument`.

    Raises:
        TypeError: If `items` is neither, one of the pairs is not a sequence of two, or a name is not text.
    """
    pairs: Iterable[Sequence[object]]
    if isinstance(items, Mapping):
        pairs = cast(Mapping[object, object], items).items()
    elif isinstance(items, Iterable) and not isinstance(items, str | Buffer):
        pairs = cast(Iterable[Sequence[object]], items)
    else:
        raise TypeError(f"{argument} has to be a mapping or (name, value) pairs, not {type(items).__name__}")
    for pair in pairs:
        match pair:
            case (str() as name, item):
                yield name, item
            case (name, _):
                raise TypeError(f"a name in {argument} has to be text, not {type(name).__name__}")
            case _:
                raise TypeError(f"an item of {argument} has to be a (name, value) pair, not {type(pair).__name__}")


def _encode_component(text: str) -> str:
    # The application/x-www-form-urlencoded set keeps "*" and encodes "~", which quote() does the other way round.
    return quote_plus(_well_formed(text), safe="*").replace("~", "%7E")


def _well_formed(text: str) -> str:
    """Return `text` as Web IDL hands it on, with U+FFFD for a lone surrogate, where encoding as UTF-8 would fail."""
    return text.encode("utf-16-le", "surrogatepass").decode("utf-16-le", "replace")


def _encode_json(value: object) -> bytes:
    try:
        text = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    except TypeError as error:
        # What json says names the type it has no form of, and never a value.
        raise TypeError(f"json has to hold only what JSON has a form of: {error}") from error
    return _LONE_SURROGATE.sub(lambda surrogate: f"\\u{ord(surrogate[0]):04x}", text).encode()


def _fields_beside_files(data: RequestData | None) -> Mapping[str, str]:
    match data:
        case None:
            return {}
        case Mapping():
            return cast(Mapping[str, str], data)
        case _:
            raise ValueError("beside files, data has to be a mapping of text fields")


def _encode_multipart(fields: Mapping[str, str], files: FileFields) -> EncodedBody:
    """Return a multipart body, as FormData::EncodeMultiPartFormData() in Blink writes it."""
    boundary = _new_boundary()
    parts: list[bytes] = []
    for name, value in _text_fields(fields, "data"):
        parts += [_part_head(boundary, name), _LINE_BREAK.sub("\r\n", _well_formed(value)).encode(), b"\r\n"]
    for name, file in _named_items(files, "files"):
        file_name, content, media_type = _file_part(name, file)
        parts += [_part_head(boundary, name, file_name, media_type), content, b"\r\n"]
    parts.append(f"--{boundary}--\r\n".encode())
    return EncodedBody(b"".join(parts), BodyEncoding.MULTIPART, boundary)


def _new_boundary() -> str:
    random_bytes = secrets.token_bytes(_BOUNDARY_RANDOM_CHARACTERS)
    return _BOUNDARY_PREFIX + "".join(_BOUNDARY_ALPHABET[byte & _LOW_SIX_BITS] for byte in random_bytes)


def _file_part(name: str, file: object) -> tuple[str, bytes, str]:
    """Return the file name, content and media type of the file in field `name`.

    Raises:
        TypeError: If it has another form. The message leaves out the file, whose content may be a secret.
    """
    match file:
        case bytes():
            return _BLOB_FILE_NAME, file, ""
        case (str() as file_name, bytes() as content):
            return file_name, content, ""
        case (str() as file_name, bytes() as content, str() as media_type):
            return file_name, content, media_type
        case _:
            raise TypeError(
                f"the file {name!r} in files has to be its content in bytes, (file name, content) or"
                f" (file name, content, media type), not {type(file).__name__}"
            )


def _part_head(boundary: str, name: str, file_name: str | None = None, media_type: str = "") -> bytes:
    lines = f'--{boundary}\r\nContent-Disposition: form-data; name="{_escape_name(name)}"'
    if file_name is not None:
        lines += f'; filename="{_escape_file_name(file_name)}"\r\nContent-Type: {_blob_type(media_type)}'
    return f"{lines}\r\n\r\n".encode()


def _escape_name(name: str) -> str:
    return _LINE_BREAK.sub("%0D%0A", _well_formed(name)).replace('"', "%22")


def _escape_file_name(file_name: str) -> str:
    return _well_formed(file_name).replace("\n", "%0A").replace("\r", "%0D").replace('"', "%22")


def _blob_type(media_type: str) -> str:
    """Return what a Blob makes of a type, in lower case, with no type at all for what could break out of its line."""
    return media_type.lower() if _MEDIA_TYPE.fullmatch(media_type) else _NO_MEDIA_TYPE
