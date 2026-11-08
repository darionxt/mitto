"""Reads what a Windows DLL exports and which DLLs it loads, from its PE headers, with no tool of Windows' own.

Only what the tests of the native library need: a 64-bit DLL, its export table, and its import and delay import tables.
"""

import struct
from dataclasses import dataclass
from pathlib import Path

__all__ = ["PeFile"]

# The signature at the offset that the DOS header names, and the magic of a 64-bit optional header.
_SIGNATURE = b"PE\0\0"
_PE32_PLUS = 0x20B

# The size of the COFF file header that follows the signature, and of one section header.
_FILE_HEADER_SIZE = 20
_SECTION_HEADER_SIZE = 40

# Where the data directories start in a 64-bit optional header, and the index of each one read here.
_DATA_DIRECTORIES_OFFSET = 112
_EXPORT_DIRECTORY = 0
_IMPORT_DIRECTORY = 1
_DELAY_IMPORT_DIRECTORY = 13

# The size of one entry of the import and the delay import tables, each of which ends with an entry of zeros.
_IMPORT_DESCRIPTOR_SIZE = 20
_DELAY_IMPORT_DESCRIPTOR_SIZE = 32


@dataclass(frozen=True)
class _Section:
    virtual_address: int
    virtual_size: int
    raw_offset: int


class PeFile:
    """A 64-bit Windows DLL, read whole from disk."""

    def __init__(self, path: Path) -> None:
        """Read the file at `path`.

        Raises:
            ValueError: If it is not a 64-bit PE file.
        """
        self._data = path.read_bytes()
        (pe_offset,) = struct.unpack_from("<I", self._data, 0x3C)
        if self._data[pe_offset : pe_offset + 4] != _SIGNATURE:
            raise ValueError(f"{path} is not a PE file")
        file_header = pe_offset + 4
        section_count, optional_header_size = struct.unpack_from("<H12xH", self._data, file_header + 2)
        optional_header = file_header + _FILE_HEADER_SIZE
        (magic,) = struct.unpack_from("<H", self._data, optional_header)
        if magic != _PE32_PLUS:
            raise ValueError(f"{path} is not a 64-bit PE file")
        self._directories_offset = optional_header + _DATA_DIRECTORIES_OFFSET
        first_section = optional_header + optional_header_size
        self._sections: list[_Section] = []
        for index in range(section_count):
            virtual_size, virtual_address, _, raw_offset = struct.unpack_from(
                "<8xIIII", self._data, first_section + index * _SECTION_HEADER_SIZE
            )
            self._sections.append(_Section(virtual_address, virtual_size, raw_offset))

    def exported_names(self) -> list[str]:
        """Return the name of each function the DLL exports, in the order of its table.

        Raises:
            ValueError: If the DLL exports a function by number alone, which has no name to return.
        """
        rva, _ = self._directory(_EXPORT_DIRECTORY)
        if rva == 0:
            return []
        offset = self._offset(rva)
        function_count, name_count, _, names_rva = struct.unpack_from("<IIII", self._data, offset + 20)
        if function_count != name_count:
            raise ValueError(f"{function_count - name_count} of the exports have no name")
        names_offset = self._offset(names_rva)
        return [
            self._string(struct.unpack_from("<I", self._data, names_offset + 4 * index)[0])
            for index in range(name_count)
        ]

    def imported_libraries(self) -> set[str]:
        """Return the names of the DLLs that this one loads, at once or when first called, in lower case."""
        names: set[str] = set()
        for directory, entry_size, name_field in (
            (_IMPORT_DIRECTORY, _IMPORT_DESCRIPTOR_SIZE, 12),
            (_DELAY_IMPORT_DIRECTORY, _DELAY_IMPORT_DESCRIPTOR_SIZE, 4),
        ):
            rva, _ = self._directory(directory)
            if rva == 0:
                continue
            entry = self._offset(rva)
            while any(self._data[entry : entry + entry_size]):
                (name_rva,) = struct.unpack_from("<I", self._data, entry + name_field)
                names.add(self._string(name_rva).lower())
                entry += entry_size
        return names

    def _directory(self, index: int) -> tuple[int, int]:
        rva, size = struct.unpack_from("<II", self._data, self._directories_offset + 8 * index)
        return rva, size

    def _offset(self, rva: int) -> int:
        for section in self._sections:
            if section.virtual_address <= rva < section.virtual_address + section.virtual_size:
                return section.raw_offset + rva - section.virtual_address
        raise ValueError(f"no section holds the address {rva:#x}")

    def _string(self, rva: int) -> str:
        start = self._offset(rva)
        return self._data[start : self._data.index(b"\0", start)].decode("ascii")
