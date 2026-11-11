"""The C++ type of each Web IDL type, and the code that converts a value between V8 and C++.

Conversions follow the Web IDL standard's JavaScript binding as Blink implements it: NativeValueTraits and ToV8Traits
for each type, and the messages of ExceptionMessages when a value does not convert. The writer handles the types that
the interfaces it writes use, and refuses the others, naming them.

No value can be an object of an interface that has no class in the Runtime, so a union loses such members before it
converts: (DOMString or TrustedScript) converts as DOMString does, and an overload that needs such an object can never
be chosen.
"""

# Upstream: third_party/blink/renderer/bindings/core/v8/native_value_traits_impl.h
# Upstream: third_party/blink/renderer/bindings/core/v8/to_v8_traits.h
# Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.h
# Upstream: third_party/blink/renderer/platform/bindings/exception_messages.cc

import dataclasses

from bindings_generator.cpp_names import file_name
from bindings_generator.errors import UnhandledError
from bindings_generator.model import IdlType, TypeKind

__all__ = ["Site", "TypeMapper", "header_of", "union_name"]

_INTEGERS = {
    "byte": "int8_t",
    "octet": "uint8_t",
    "short": "int16_t",
    "unsigned short": "uint16_t",
    "long": "int32_t",
    "unsigned long": "uint32_t",
    "long long": "int64_t",
    "unsigned long long": "uint64_t",
}

# The integer types whose conversion from a JavaScript value the writer handles: ToInt32 or ToUint32 and then, for the
# narrower ones, the low bits, which is what Web IDL's ConvertToInt() gives without [EnforceRange] or [Clamp].
_CONVERTED_INTEGERS = frozenset({"byte", "octet", "short", "unsigned short", "long", "unsigned long"})

# The conversion annotations on a type that the writer follows.
_ANNOTATIONS = frozenset({"LegacyNullToEmptyString"})


def union_name(idl_type: IdlType) -> str:
    """The C++ name of a union, as Blink's union_name_mapper.py names it, such as AddEventListenerOptionsOrBoolean."""
    return "Or".join(member.name[0].upper() + member.name[1:] for member in idl_type.members)


@dataclasses.dataclass(frozen=True)
class Site:
    """Where a value converts, which decides the message when it does not.

    Attributes:
        fail: the statement that leaves after an exception, such as "return;".
        argument_index: the argument's index, for an argument.
        argument_index_expression: the C++ expression of the index, for a variadic argument.
        dictionary: the dictionary, for a dictionary member.
        member: the member's name, for a dictionary member.
    """

    fail: str
    argument_index: int | None = None
    argument_index_expression: str | None = None
    dictionary: str | None = None
    member: str | None = None

    def not_of_type(self, type_name: str) -> str:
        """The expression of the message for a value that is not of the type."""
        if self.argument_index_expression is not None:
            return f"ExceptionMessages::ArgumentNotOfType({self.argument_index_expression}, {_quote(type_name)})"
        if self.argument_index is not None:
            return f"ExceptionMessages::ArgumentNotOfType({self.argument_index}, {_quote(type_name)})"
        if self.dictionary is not None:
            # The member's context goes in front when the error leaves the call (exception_context.h).
            return f"ExceptionMessages::FailedToConvertJSValue({_quote(type_name)})"
        return f"ExceptionMessages::ValueNotOfType({_quote(type_name)})"


def _quote(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


class TypeMapper:
    """Maps Web IDL types to C++, for the interfaces that have a C++ class.

    Args:
        implemented: the interfaces that have a C++ class, which bindings can convert to and from.
        dictionaries: the dictionaries that the database holds.
    """

    def __init__(self, implemented: frozenset[str], dictionaries: frozenset[str]) -> None:
        self._implemented = implemented
        self._dictionaries = dictionaries
        self.used_dictionaries: list[str] = []
        self.used_unions: dict[str, IdlType] = {}
        self.used_interfaces: set[str] = set()

    def has_class(self, interface: str) -> bool:
        """Whether the interface has a C++ class."""
        return interface in self._implemented

    def can_hold_a_value(self, idl_type: IdlType) -> bool:
        """Whether a value other than null converts to the type: not for an interface without a class."""
        reduced = self.reduce(idl_type)
        if reduced.kind is TypeKind.INTERFACE:
            return reduced.name in self._implemented
        if reduced.kind is TypeKind.UNION:
            return bool(reduced.members) or reduced.nullable
        return True

    def reduce(self, idl_type: IdlType) -> IdlType:
        """The type without the members of a union that no value can be, those of interfaces without a class.

        A union left with one member is that member, nullable if the union was.
        """
        if idl_type.kind is not TypeKind.UNION:
            return idl_type
        members: list[IdlType] = []
        for member in idl_type.members:
            flat = self.reduce(member)
            if flat.kind is TypeKind.UNION:
                members += flat.members
            elif not (flat.kind is TypeKind.INTERFACE and flat.name not in self._implemented):
                members.append(flat)
        if len(members) == 1:
            only = members[0]
            return dataclasses.replace(only, nullable=only.nullable or idl_type.nullable)
        return dataclasses.replace(
            idl_type, members=tuple(members), name=" or ".join(member.syntax for member in members)
        )

    def _refuse(self, idl_type: IdlType, where: str, use: str) -> UnhandledError:
        return UnhandledError(f"{where}: the C++ writer does not handle {use} of type {idl_type.syntax}")

    def _check_annotations(self, idl_type: IdlType, where: str) -> None:
        for key, _ in idl_type.extended_attributes.entries:
            if key not in _ANNOTATIONS or idl_type.kind is not TypeKind.STRING:
                raise UnhandledError(f"{where}: the C++ writer does not handle [{key}] on the type {idl_type.syntax}")

    def _use_dictionary(self, name: str) -> None:
        if name not in self.used_dictionaries:
            self.used_dictionaries.append(name)

    def argument_type(self, idl_type: IdlType, where: str) -> str:
        """The C++ type that a class's method takes for a value of the type."""
        idl_type = self.reduce(idl_type)
        self._check_annotations(idl_type, where)
        kind = idl_type.kind
        if kind is TypeKind.BOOLEAN and not idl_type.nullable:
            return "bool"
        if kind is TypeKind.INTEGER and idl_type.name in _CONVERTED_INTEGERS and not idl_type.nullable:
            return _INTEGERS[idl_type.name]
        if kind is TypeKind.STRING and idl_type.name in ("DOMString", "USVString"):
            if idl_type.nullable:
                return "const std::optional<std::u16string>&"
            return "const std::u16string&"
        if kind is TypeKind.ANY:
            return "v8::Local<v8::Value>"
        if kind is TypeKind.INTERFACE and idl_type.name in self._implemented:
            self.used_interfaces.add(idl_type.name)
            return f"mitto::{idl_type.name}*"
        if kind is TypeKind.CALLBACK_INTERFACE:
            return "v8::Local<v8::Object>"
        if kind is TypeKind.DICTIONARY and not idl_type.nullable:
            self._use_dictionary(idl_type.name)
            return f"const {idl_type.name}&"
        if kind is TypeKind.UNION and not idl_type.nullable:
            self._register_union(idl_type, where)
            return f"const {union_name(idl_type)}&"
        raise self._refuse(idl_type, where, "an argument")

    def value_type(self, idl_type: IdlType, where: str) -> str:
        """The C++ type that holds a converted value of the type."""
        return self.argument_type(idl_type, where).removeprefix("const ").removesuffix("&")

    def return_type(self, idl_type: IdlType, where: str) -> str:
        """The C++ type that a class's method returns for the type."""
        idl_type = self.reduce(idl_type)
        kind = idl_type.kind
        nullable_ok = kind in (TypeKind.INTERFACE, TypeKind.ANY, TypeKind.STRING)
        if idl_type.nullable and not nullable_ok:
            raise self._refuse(idl_type, where, "a return value")
        if kind is TypeKind.UNDEFINED:
            return "void"
        if kind is TypeKind.BOOLEAN:
            return "bool"
        if kind is TypeKind.INTEGER:
            return _INTEGERS[idl_type.name]
        if kind is TypeKind.FLOATING_POINT:
            return "float" if idl_type.name.endswith("float") else "double"
        if kind is TypeKind.STRING and idl_type.name in ("DOMString", "USVString"):
            return "std::optional<std::u16string>" if idl_type.nullable else "std::u16string"
        if kind is TypeKind.ENUMERATION:
            # The value as a page sees it, one of the enumeration's strings, where Blink returns its V8 enum class.
            return "std::u16string"
        if kind is TypeKind.ANY:
            return "v8::Local<v8::Value>"
        if kind is TypeKind.INTERFACE and idl_type.name in self._implemented:
            self.used_interfaces.add(idl_type.name)
            return f"mitto::{idl_type.name}*"
        if kind is TypeKind.INTERFACE and idl_type.nullable:
            # No object can be of an interface without a class, so the class can only give null.
            return "bindings::ScriptWrappable*"
        if kind is TypeKind.SEQUENCE:
            element = idl_type.members[0]
            if element.kind is TypeKind.INTERFACE and element.name in self._implemented and not element.nullable:
                self.used_interfaces.add(element.name)
                return f"std::vector<mitto::{element.name}*>"
            if element.kind is TypeKind.STRING and element.name == "DOMString" and not element.nullable:
                return "std::vector<std::u16string>"
        raise self._refuse(idl_type, where, "a return value")

    def to_v8(self, idl_type: IdlType, expression: str) -> str:
        """The expression that converts a C++ value, which return_type() accepted, to V8."""
        idl_type = self.reduce(idl_type)
        kind = idl_type.kind
        if kind is TypeKind.BOOLEAN:
            return f"v8::Boolean::New(isolate, {expression})"
        if kind is TypeKind.INTEGER:
            if _INTEGERS[idl_type.name].startswith("u"):
                return f"v8::Integer::NewFromUnsigned(isolate, {expression})"
            return f"v8::Integer::New(isolate, {expression})"
        if kind is TypeKind.FLOATING_POINT:
            return f"v8::Number::New(isolate, {expression})"
        if kind is TypeKind.STRING:
            if idl_type.nullable:
                return f"ToV8NullableString(isolate, {expression})"
            return f"ToV8String(isolate, {expression})"
        if kind is TypeKind.ENUMERATION:
            return f"ToV8String(isolate, {expression})"
        if kind is TypeKind.ANY:
            return expression
        if kind is TypeKind.INTERFACE:
            return f"ToV8(isolate, {expression})"
        if kind is TypeKind.SEQUENCE:
            if idl_type.members[0].kind is TypeKind.STRING:
                return f"ToV8StringArray(isolate, {expression})"
            return f"ToV8Array(isolate, {expression})"
        raise AssertionError(idl_type.syntax)

    def _register_union(self, idl_type: IdlType, where: str) -> None:
        handled = (TypeKind.BOOLEAN, TypeKind.DICTIONARY, TypeKind.STRING, TypeKind.INTERFACE)
        for member in idl_type.members:
            if member.kind not in handled or member.nullable or member.extended_attributes.entries:
                raise self._refuse(idl_type, where, "a union")
            if member.kind is TypeKind.STRING and member.name != "DOMString":
                raise self._refuse(idl_type, where, "a union")
        # Interfaces with a class can be any number. Of the others, Web IDL's conversion takes the first that fits, so
        # the writer allows one of each.
        kinds = [member.kind for member in idl_type.members if member.kind is not TypeKind.INTERFACE]
        if len(set(kinds)) != len(kinds):
            raise self._refuse(idl_type, where, "a union")
        for member in idl_type.members:
            if member.kind is TypeKind.DICTIONARY:
                self._use_dictionary(member.name)
            if member.kind is TypeKind.INTERFACE:
                self.used_interfaces.add(member.name)
        self.used_unions.setdefault(union_name(idl_type), idl_type)

    def union_alternative(self, member: IdlType) -> str:
        """The C++ type of a member of a union, in its std::variant."""
        if member.kind is TypeKind.BOOLEAN:
            return "bool"
        if member.kind is TypeKind.STRING:
            return "std::u16string"
        if member.kind is TypeKind.INTERFACE:
            return f"mitto::{member.name}*"
        return member.name

    def from_v8(self, idl_type: IdlType, value: str, target: str, site: Site, where: str) -> list[str]:
        """The statements that convert `value` to a new variable `target`, and leave with site.fail if that throws."""
        idl_type = self.reduce(idl_type)
        kind = idl_type.kind
        if not self.can_hold_a_value(idl_type):
            # Nothing can be an instance of an interface without a class, so any value but null fails to convert.
            if site.dictionary is None:
                raise self._refuse(idl_type, where, "an argument")
            fail = [f"exception_state.ThrowTypeError({site.not_of_type(idl_type.name)});", site.fail]
            if idl_type.nullable:
                return [f"if (!{value}->IsNull()) {{", *["  " + line for line in fail], "}"]
            return fail
        cpp_type = self.value_type(idl_type, where)
        if kind is TypeKind.BOOLEAN:
            return [f"const bool {target} = {value}->BooleanValue(isolate);"]
        if kind is TypeKind.INTEGER:
            return [
                f"{cpp_type} {target};",
                f"if (!FromV8Integer(isolate, {value}, &{target})) {{",
                f"  {site.fail}",
                "}",
            ]
        if kind is TypeKind.STRING:
            if "LegacyNullToEmptyString" in idl_type.extended_attributes:
                function = "FromV8StringNullToEmpty"
            elif idl_type.nullable:
                function = "FromV8NullableUSVString" if idl_type.name == "USVString" else "FromV8NullableString"
            else:
                function = "FromV8USVString" if idl_type.name == "USVString" else "FromV8String"
            return [
                f"{cpp_type} {target};",
                f"if (!{function}(isolate, {value}, &{target})) {{",
                f"  {site.fail}",
                "}",
            ]
        if kind is TypeKind.ANY:
            return [f"const v8::Local<v8::Value> {target} = {value};"]
        if kind is TypeKind.INTERFACE:
            check = f"!{target}"
            if idl_type.nullable:
                check += f" && !{value}->IsNullOrUndefined()"
            return [
                f"{cpp_type} const {target} = V8{idl_type.name}::ToWrappable(isolate, {value});",
                f"if ({check}) {{",
                f"  exception_state.ThrowTypeError({site.not_of_type(idl_type.name)});",
                f"  {site.fail}",
                "}",
            ]
        if kind is TypeKind.CALLBACK_INTERFACE:
            lines = [
                f"v8::Local<v8::Object> {target};",
                f"if ({value}->IsObject()) {{",
                f"  {target} = {value}.As<v8::Object>();",
            ]
            if idl_type.nullable:
                lines.append(f"}} else if (!{value}->IsNullOrUndefined()) {{")
            else:
                lines.append("} else {")
            lines += [f"  exception_state.ThrowTypeError({site.not_of_type('Object')});", f"  {site.fail}", "}"]
            return lines
        if kind in (TypeKind.DICTIONARY, TypeKind.UNION):
            return [
                f"{cpp_type} {target};",
                f"if (!FromV8(isolate, {value}, exception_state, &{target})) {{",
                f"  {site.fail}",
                "}",
            ]
        raise self._refuse(idl_type, where, "a value")

    def default_value(self, idl_type: IdlType, literal: str, where: str) -> str:
        """The C++ expression of a default value, as the IDL file writes it."""
        idl_type = self.reduce(idl_type)
        kind = idl_type.kind
        if kind is TypeKind.BOOLEAN and literal in ("true", "false"):
            return literal
        if kind is TypeKind.STRING and literal.startswith('"'):
            return "u" + literal
        if kind is TypeKind.STRING and idl_type.nullable and literal == "null":
            return "std::nullopt"
        if kind is TypeKind.DICTIONARY and literal == "{}":
            return f"{idl_type.name}()"
        if kind is TypeKind.ANY and literal == "null":
            return "v8::Null(isolate)"
        raise UnhandledError(
            f"{where}: the C++ writer does not handle the default value {literal} of {idl_type.syntax}"
        )


def header_of(interface: str) -> str:
    """The generated header of an interface's bindings."""
    return f"mitto/bindings/v8_{file_name(interface)}.h"
