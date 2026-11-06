"""Encoding a request's body as Chrome does, with no I/O.

A mapping follows URLSearchParams, and a multipart body follows Blink's FormDataEncoder. What a body adds to a request's
headers comes from the Persona, as docs/persona.md says.
"""

import json
import re
import secrets
from collections.abc import Buffer, Iterable, Mapping
from dataclasses import dataclass, field
from enum import StrEnum
from typing import cast
from urllib.parse import quote_plus

__all__ = [
    "BodyEncoding",
    "EncodedBody",
    "FileFields",
    "FilePart",
    "QueryParams",
    "RequestData",
    "encode_body",
    "encode_query",
]

type QueryParams = Mapping[str, str] | Iterable[tuple[str, str]]
"""A query as names and values. Pairs can repeat a name."""

type RequestData = Buffer | str | Mapping[str, str]
"""A body as bytes, as text, or as form fields by name."""

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


def encode_body(data: RequestData | None, json_value: object, files: FileFields | None) -> EncodedBody | None:
    """Return the body that a caller's arguments make, or None if they make none.

    Args:
        data: Bytes go as they are. Text goes as UTF-8. A mapping is encoded as URLSearchParams does, or with `files`, it
            holds the text fields of the multipart body.
        json_value: The value to write as JSON.stringify does, or None for none. So it writes no JSON null, which `data`
            can send.
        files: The files of a multipart body, which follow any text fields in `data`.

    Raises:
        TypeError: If `data` is of no type that makes a body, or `json_value` holds a value that JSON has no form of.
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
            return EncodedBody(encode_query(data).encode(), BodyEncoding.FORM)
        case Buffer():
            return EncodedBody(bytes(data))
        case _:
            raise TypeError(f"data has to be bytes, text or a mapping, not {type(data).__name__}")


def encode_query(params: QueryParams) -> str:
    """Return `params` as the text of a query or a form's body, as URLSearchParams.toString() does."""
    pairs = cast(Mapping[str, str], params).items() if isinstance(params, Mapping) else params
    return "&".join(f"{_encode_component(name)}={_encode_component(value)}" for name, value in pairs)


def _encode_component(text: str) -> str:
    # The application/x-www-form-urlencoded set keeps "*" and encodes "~", which quote() does the other way round.
    return quote_plus(_well_formed(text), safe="*").replace("~", "%7E")


def _well_formed(text: str) -> str:
    """Return `text` as Web IDL hands it on, with U+FFFD for a lone surrogate, where encoding as UTF-8 would fail."""
    return text.encode("utf-16-le", "surrogatepass").decode("utf-16-le", "replace")


def _encode_json(value: object) -> bytes:
    text = json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    return _LONE_SURROGATE.sub(lambda surrogate: f"\\u{ord(surrogate[0]):04x}", text).encode()


def _fields_beside_files(data: RequestData | None) -> Mapping[str, str]:
    match data:
        case None:
            return {}
        case Mapping():
            return data
        case _:
            raise ValueError("beside files, data has to be a mapping of text fields")


def _encode_multipart(fields: Mapping[str, str], files: FileFields) -> EncodedBody:
    """Return a multipart body, as FormData::EncodeMultiPartFormData() in Blink writes it."""
    boundary = _new_boundary()
    parts: list[bytes] = []
    for name, value in fields.items():
        parts += [_part_head(boundary, name), _LINE_BREAK.sub("\r\n", _well_formed(value)).encode(), b"\r\n"]
    pairs = cast(Mapping[str, FilePart], files).items() if isinstance(files, Mapping) else files
    for name, file in pairs:
        file_name, content, media_type = _file_part(file)
        parts += [_part_head(boundary, name, file_name, media_type), content, b"\r\n"]
    parts.append(f"--{boundary}--\r\n".encode())
    return EncodedBody(b"".join(parts), BodyEncoding.MULTIPART, boundary)


def _new_boundary() -> str:
    random_bytes = secrets.token_bytes(_BOUNDARY_RANDOM_CHARACTERS)
    return _BOUNDARY_PREFIX + "".join(_BOUNDARY_ALPHABET[byte & _LOW_SIX_BITS] for byte in random_bytes)


def _file_part(file: FilePart) -> tuple[str, bytes, str]:
    match file:
        case bytes():
            return _BLOB_FILE_NAME, file, ""
        case (str() as file_name, bytes() as content):
            return file_name, content, ""
        case (str() as file_name, bytes() as content, str() as media_type):
            return file_name, content, media_type
        case _:
            raise TypeError("a file has to be its content in bytes, (name, content) or (name, content, media type)")


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
