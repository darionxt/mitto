"""Writes Web IDL types, dictionaries and operations as data, for the members whose behaviour the Runtime lacks.

A Known Gap converts its arguments as Chrome's binding does and then gives its type's default (docs/runtime.md, "What a
Known Gap does"). native/bindings/known_gap.cc does both at run time from the tables that this module writes into
idl_types.cc: one IdlTypeInfo for each distinct type, one DictionaryInfo for each dictionary they reach, and one
OperationInfo for each operation or constructor that is a Known Gap. idl_types.h declares them for the tables of the
interfaces and the bindings of the classes that name them.
"""

# Upstream: third_party/blink/renderer/bindings/scripts/web_idl/idl_type.py
# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/union.py

from collections.abc import Callable

from bindings_generator.errors import UnhandledError
from bindings_generator.model import Condition, Dictionary, IdlType, OperationGroup, TypeKind

__all__ = ["HEADER", "SOURCE", "ConditionFunctions", "TypeTables", "union_name_in_idl"]

HEADER = "idl_types.h"
SOURCE = "idl_types.cc"

_BANNER = "// Written by the bindings generator in native/bindings from Blink's Web IDL\n// database. Do not edit.\n"

_SIMPLE_KINDS = {
    TypeKind.ANY: "kAny",
    TypeKind.UNDEFINED: "kUndefined",
    TypeKind.BOOLEAN: "kBoolean",
    TypeKind.BIGINT: "kBigInt",
    TypeKind.OBJECT: "kObject",
    TypeKind.SYMBOL: "kSymbol",
    TypeKind.INTERFACE: "kInterface",
    TypeKind.CALLBACK_INTERFACE: "kCallbackInterface",
    TypeKind.CALLBACK_FUNCTION: "kCallbackFunction",
    TypeKind.DICTIONARY: "kDictionary",
    TypeKind.ENUMERATION: "kEnumeration",
    TypeKind.SEQUENCE: "kSequence",
    TypeKind.FROZEN_ARRAY: "kFrozenArray",
    TypeKind.OBSERVABLE_ARRAY: "kObservableArray",
    TypeKind.RECORD: "kRecord",
    TypeKind.PROMISE: "kPromise",
    TypeKind.UNION: "kUnion",
    TypeKind.ITERATOR: "kIterator",
}

_NAMED_KINDS = {
    "byte": "kByte",
    "octet": "kOctet",
    "short": "kShort",
    "unsigned short": "kUnsignedShort",
    "long": "kLong",
    "unsigned long": "kUnsignedLong",
    "long long": "kLongLong",
    "unsigned long long": "kUnsignedLongLong",
    "float": "kFloat",
    "unrestricted float": "kUnrestrictedFloat",
    "double": "kDouble",
    "unrestricted double": "kUnrestrictedDouble",
    "DOMString": "kDOMString",
    "ByteString": "kByteString",
    "USVString": "kUSVString",
    "ArrayBuffer": "kArrayBuffer",
    "SharedArrayBuffer": "kSharedArrayBuffer",
    "ArrayBufferView": "kArrayBufferView",
    "DataView": "kDataView",
}

_TYPED_ARRAYS = frozenset(
    {
        "Int8Array",
        "Int16Array",
        "Int32Array",
        "Uint8Array",
        "Uint16Array",
        "Uint32Array",
        "Uint8ClampedArray",
        "BigInt64Array",
        "BigUint64Array",
        "Float16Array",
        "Float32Array",
        "Float64Array",
    }
)

# The conversion annotations on a type and the flag each sets, or None for one that changes nothing a page sees of a
# conversion from JavaScript.
_ANNOTATIONS: dict[str, str | None] = {
    "ConvertibleToObject": None,
    "EnforceRange": "kIdlTypeEnforceRange",
    "Clamp": "kIdlTypeClamp",
    "LegacyNullToEmptyString": "kIdlTypeLegacyNullToEmptyString",
    "AllowShared": "kIdlTypeAllowShared",
    "BufferSourceTypeNoSizeLimit": "kIdlTypeAllowResizable",
    "PassAsSpan": "kIdlTypeAllowResizable",
}


def union_name_in_idl(union: IdlType) -> str:
    """The union as bind_gen's UnionNameInIDL() writes it for its errors: its members' names, sorted."""
    names = " or ".join(sorted(member.syntax for member in union.members))
    return f"({names}){'?' if union.nullable else ''}"


def _quote(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


class ConditionFunctions:
    """The C++ functions that test conditions, one for each distinct test, in one file."""

    def __init__(self, condition_test: Callable[[Condition, str], str | None]) -> None:
        self._condition_test = condition_test
        self._names: dict[str, str] = {}
        self.lines: list[str] = []

    def reference(self, condition: Condition, where: str) -> str | None:
        """The function's address, "nullptr" for always, or None for never."""
        test = self._condition_test(condition, where)
        if test is None:
            return None
        if test == "true":
            return "nullptr"
        if test not in self._names:
            index = len(self._names)
            self._names[test] = f"&Condition{index}"
            self.lines += [
                f"bool Condition{index}(const PerIsolateData& isolate_data) {{",
                "  [[maybe_unused]] const PerIsolateData* const data = &isolate_data;",
                "  [[maybe_unused]] const RuntimeFeatureSet& features = isolate_data.features();",
                f"  return {test};",
                "}",
                "",
            ]
        return self._names[test]


class TypeTables:
    """Collects the types, dictionaries and operations that Known Gaps name, and writes them.

    Args:
        dictionaries: the dictionaries of the database.
        enumerations: the enumerations of the database.
        non_object_as_null_callbacks: the callback functions with [LegacyTreatNonObjectAsNull].
        info_of: the expression of an interface's WrapperTypeInfo, or None if it has none.
        condition_test: the C++ test of a condition, as cpp_writer.condition_test() writes it.
    """

    def __init__(
        self,
        dictionaries: dict[str, Dictionary],
        enumerations: dict[str, tuple[str, ...]],
        non_object_as_null_callbacks: frozenset[str],
        info_of: Callable[[str], str | None],
        condition_test: Callable[[Condition, str], str | None],
    ) -> None:
        self._dictionaries = dictionaries
        self._enumerations = enumerations
        self._non_object_as_null = non_object_as_null_callbacks
        self._info_of = info_of
        self._conditions = ConditionFunctions(condition_test)
        self._types: dict[tuple[object, ...], str] = {}
        self._type_lines: list[str] = []
        self._dictionary_names: dict[str, str] = {}
        self._dictionary_lines: list[str] = []
        self._operations: dict[tuple[object, ...], str] = {}
        self._operation_lines: list[str] = []
        self._declarations: list[str] = []

    def type(self, idl_type: IdlType, where: str) -> str:
        """The address of the type's IdlTypeInfo."""
        flags: list[str] = []
        if idl_type.nullable:
            flags.append("kIdlTypeNullable")
        for key, _ in idl_type.extended_attributes.entries:
            if key not in _ANNOTATIONS:
                raise UnhandledError(f"{where}: Known Gaps do not handle [{key}] on the type {idl_type.syntax}")
            flag = _ANNOTATIONS[key]
            if flag is not None:
                flags.append(flag)
        kind = idl_type.kind
        if kind in _SIMPLE_KINDS:
            cpp_kind = _SIMPLE_KINDS[kind]
        elif idl_type.name in _NAMED_KINDS:
            cpp_kind = _NAMED_KINDS[idl_type.name]
        elif kind is TypeKind.BUFFER_SOURCE and idl_type.name in _TYPED_ARRAYS:
            cpp_kind = "kTypedArray"
        else:
            raise UnhandledError(f"{where}: Known Gaps do not handle the type {idl_type.syntax}")
        if kind is TypeKind.CALLBACK_FUNCTION and idl_type.name in self._non_object_as_null:
            flags.append("kIdlTypeLegacyTreatNonObjectAsNull")
        members = tuple(self.type(member, where) for member in idl_type.members)
        if kind is TypeKind.UNION:
            name = union_name_in_idl(idl_type).removesuffix("?")
        else:
            name = idl_type.syntax.removesuffix("?")
        interface = "nullptr"
        if kind in (TypeKind.INTERFACE, TypeKind.ITERATOR):
            # An interface that a Page has no bindings for, as one exposed only to workers, has no objects there.
            interface = self._info_of(idl_type.name) or "nullptr"
        dictionary = self.dictionary(idl_type.name, where) if kind is TypeKind.DICTIONARY else "nullptr"
        if kind is TypeKind.ENUMERATION and idl_type.name not in self._enumerations:
            raise UnhandledError(f"{where}: the enumeration {idl_type.name} is not in the database")
        key = (cpp_kind, name, tuple(sorted(set(flags))), interface, dictionary, idl_type.name, members)
        if key in self._types:
            return self._types[key]
        index = len(self._types)
        values = "{}"
        if kind is TypeKind.ENUMERATION:
            values = f"kValues{index}"
            items = ", ".join(_quote(value) for value in self._enumerations[idl_type.name])
            self._type_lines.append(f"constexpr const char* {values}[] = {{{items}}};")
        member_array = "{}"
        if members:
            member_array = f"kMembers{index}"
            self._type_lines.append(f"constexpr const IdlTypeInfo* {member_array}[] = {{{', '.join(members)}}};")
        self._types[key] = f"&kIdlType{index}"
        self._declarations.append(f"extern const IdlTypeInfo kIdlType{index};")
        self._type_lines += [
            f"const IdlTypeInfo kIdlType{index} = {{{_quote(name)}, IdlTypeKind::{cpp_kind}, "
            f"{' | '.join(sorted(set(flags))) or '0'}, {interface}, {dictionary}, {values}, {member_array}}};",
        ]
        return self._types[key]

    def dictionary(self, name: str, where: str) -> str:
        """The address of the dictionary's DictionaryInfo."""
        if name in self._dictionary_names:
            return self._dictionary_names[name]
        dictionary = self._dictionaries.get(name)
        if dictionary is None:
            raise UnhandledError(f"{where}: the dictionary {name} is not in the database")
        reference = f"&kDictionary{len(self._dictionary_names)}"
        self._dictionary_names[name] = reference
        index = reference.removeprefix("&kDictionary")
        self._declarations.append(f"extern const DictionaryInfo kDictionary{index};")
        parent = "nullptr" if dictionary.parent is None else self.dictionary(dictionary.parent, where)
        rows: list[str] = []
        for member in dictionary.own_members:
            member_where = f"dictionary member {name}.{member.name}"
            condition = self._conditions.reference(member.condition, member_where)
            if condition is None:
                continue
            default = "nullptr" if member.default is None else _quote(member.default)
            rows.append(
                f"    {{{_quote(member.name)}, {self.type(member.idl_type, member_where)}, {condition}, "
                f"{'true' if member.is_required else 'false'}, {default}}},"
            )
        members = "{}"
        if rows:
            members = f"kDictionaryMembers{index}"
            self._dictionary_lines += [f"constexpr DictionaryMemberInfo {members}[] = {{", *rows, "};"]
        self._dictionary_lines.append(
            f"const DictionaryInfo kDictionary{index} = {{{_quote(name)}, {parent}, {members}}};"
        )
        return reference

    def operation(self, group: OperationGroup, where: str) -> str:
        """The address of the OperationInfo of an operation or constructor's overloads."""
        overloads: list[tuple[tuple[str, ...], str, str]] = []
        for overload in group.overloads:
            condition = self._conditions.reference(overload.condition, where)
            if condition is None:
                continue
            arguments: list[str] = []
            for argument in overload.arguments:
                argument_flags = [
                    flag
                    for flag, applies in (
                        ("kArgumentOptional", argument.is_optional),
                        ("kArgumentVariadic", argument.is_variadic),
                    )
                    if applies
                ]
                argument_type = self.type(argument.idl_type, f"argument {argument.name} of {where}")
                arguments.append(f"{{{argument_type}, {' | '.join(argument_flags) or '0'}}}")
            return_type = self.type(overload.return_type, f"{where} return type")
            overloads.append((tuple(arguments), return_type, condition))
        key = tuple(overloads)
        if key in self._operations:
            return self._operations[key]
        index = len(self._operations)
        rows: list[str] = []
        for overload_index, (argument_rows, return_type, condition) in enumerate(overloads):
            array = "{}"
            if argument_rows:
                # An overload's arguments go into an array of their own, which its OverloadInfo names.
                array = f"kArguments{index}_{overload_index}"
                self._operation_lines.append(f"constexpr ArgumentInfo {array}[] = {{{', '.join(argument_rows)}}};")
            rows.append(f"{{{array}, {return_type}, {condition}}}")
        overloads_array = "{}"
        if rows:
            overloads_array = f"kOverloads{index}"
            self._operation_lines.append(f"constexpr OverloadInfo {overloads_array}[] = {{{', '.join(rows)}}};")
        self._operation_lines.append(f"const OperationInfo kOperation{index} = {{{overloads_array}}};")
        self._declarations.append(f"extern const OperationInfo kOperation{index};")
        self._operations[key] = f"&kOperation{index}"
        return self._operations[key]

    def write(self, includes: set[str]) -> tuple[str, str]:
        """The header that declares every table, and the source that defines them.

        Args:
            includes: the headers that declare the WrapperTypeInfo of the interfaces the types name.
        """
        header = [
            _BANNER,
            "#ifndef MITTO_BINDINGS_IDL_TYPES_H_",
            "#define MITTO_BINDINGS_IDL_TYPES_H_",
            "",
            '#include "mitto/bindings/idl_type_info.h"',
            "",
            "namespace mitto::bindings {",
            "",
            *self._declarations,
            "",
            "}  // namespace mitto::bindings",
            "",
            "#endif  // MITTO_BINDINGS_IDL_TYPES_H_",
            "",
        ]
        source = [
            _BANNER,
            f'#include "mitto/bindings/{HEADER}"',
            "",
            *[
                f'#include "{include}"'
                for include in sorted(
                    includes
                    | {
                        "mitto/bindings/per_isolate_data.h",
                        "mitto/bindings/runtime_feature_set.h",
                        "mitto/bindings/runtime_features.h",
                    }
                )
            ],
            "",
            "namespace mitto::bindings {",
            "",
            "namespace {",
            "",
            *self._conditions.lines,
            "}  // namespace",
            "",
            *self._type_lines,
            "",
            *self._dictionary_lines,
            "",
            *self._operation_lines,
            "",
            "}  // namespace mitto::bindings",
            "",
        ]
        return "\n".join(header), "\n".join(source)
