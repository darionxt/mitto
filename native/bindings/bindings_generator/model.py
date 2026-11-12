"""The Web IDL definitions as the generator reads them from Blink's database.

The reader fills these from the database, after it has checked every construct and extended attribute, so that the
install plan and the C++ writer work on plain values and not on Blink's objects.
"""

import dataclasses
import enum

from bindings_generator.errors import UnhandledError

__all__ = [
    "ALWAYS",
    "Argument",
    "Attribute",
    "CallbackInterface",
    "CollectionLike",
    "Condition",
    "Constant",
    "Dictionary",
    "DictionaryMember",
    "ExposedConstruct",
    "ExtendedAttributes",
    "IdlType",
    "Interface",
    "Operation",
    "OperationGroup",
    "TypeKind",
]


@dataclasses.dataclass(frozen=True)
class ExtendedAttributes:
    """The extended attributes of a construct: each key with its values, in the database's order."""

    entries: tuple[tuple[str, tuple[str, ...]], ...] = ()

    def keys(self) -> list[str]:
        return [key for key, _ in self.entries]

    def __contains__(self, key: object) -> bool:
        return any(entry_key == key for entry_key, _ in self.entries)

    def values_of(self, key: str) -> tuple[str, ...]:
        for entry_key, values in self.entries:
            if entry_key == key:
                return values
        return ()

    def value_of(self, key: str) -> str | None:
        values = self.values_of(key)
        return values[0] if values else None


class TypeKind(enum.Enum):
    ANY = "any"
    UNDEFINED = "undefined"
    BOOLEAN = "boolean"
    INTEGER = "integer"
    FLOATING_POINT = "floating point"
    BIGINT = "bigint"
    STRING = "string"
    OBJECT = "object"
    SYMBOL = "symbol"
    BUFFER_SOURCE = "buffer source"
    INTERFACE = "interface"
    CALLBACK_INTERFACE = "callback interface"
    CALLBACK_FUNCTION = "callback function"
    DICTIONARY = "dictionary"
    ENUMERATION = "enumeration"
    SEQUENCE = "sequence"
    FROZEN_ARRAY = "frozen array"
    OBSERVABLE_ARRAY = "observable array"
    RECORD = "record"
    PROMISE = "promise"
    UNION = "union"
    ITERATOR = "iterator"


@dataclasses.dataclass(frozen=True)
class IdlType:
    """A Web IDL type with its typedefs resolved.

    Attributes:
        kind: what kind of type it is.
        name: the keyword of a primitive type, such as "unsigned short" or "DOMString", or the identifier of a
            definition, such as "EventTarget". Unions have their members joined by " or ".
        nullable: whether null is a value of the type.
        extended_attributes: the conversion attributes on the type, such as Clamp.
        members: the members of a union, the element type of a sequence, frozen or observable array, the result type
            of a promise, or the key and value types of a record.
    """

    kind: TypeKind
    name: str
    nullable: bool = False
    extended_attributes: ExtendedAttributes = ExtendedAttributes()
    members: tuple["IdlType", ...] = ()

    @property
    def syntax(self) -> str:
        """The type as an IDL file writes it, which Blink's messages name."""
        if self.kind is TypeKind.UNION:
            text = f"({self.name})"
        elif self.kind in (TypeKind.SEQUENCE, TypeKind.FROZEN_ARRAY, TypeKind.OBSERVABLE_ARRAY, TypeKind.PROMISE):
            text = f"{self.name}<{self.members[0].syntax}>"
        elif self.kind is TypeKind.RECORD:
            text = f"record<{self.members[0].syntax}, {self.members[1].syntax}>"
        else:
            text = self.name
        return text + "?" if self.nullable else text


@dataclasses.dataclass(frozen=True)
class Condition:
    """When a construct is exposed, beyond being on the global object it is exposed on.

    Attributes:
        features: runtime-enabled features that must all be on, which a Page checks when it builds its templates.
        other: what else decides it, as the extended attributes say it, such as "SecureContext".
        context_dependent: whether Blink decides it for each context, after the interface's templates are made, and
            so installs the construct after those that the templates hold.
        alternatives: for the overloads of one operation whose conditions differ, each overload's condition: the
            operation is installed if any of them holds, and `features` and `other` are empty.
    """

    features: tuple[str, ...] = ()
    other: tuple[str, ...] = ()
    context_dependent: bool = False
    alternatives: tuple["Condition", ...] = ()

    @property
    def is_always(self) -> bool:
        return not self.features and not self.other and not self.alternatives

    def describe(self) -> str:
        """The condition as the listing writes it, empty when there is none."""
        if self.alternatives:
            return " or ".join(f"({alternative.describe()})" for alternative in self.alternatives)
        return " ".join([f"[RuntimeEnabled={feature}]" for feature in self.features] + [f"[{c}]" for c in self.other])


ALWAYS = Condition()


@dataclasses.dataclass(frozen=True)
class Argument:
    name: str
    idl_type: IdlType
    is_optional: bool
    is_variadic: bool
    default: str | None
    """The default value as the IDL file writes it, such as "false", "{}" or "\"Error\""."""


@dataclasses.dataclass(frozen=True)
class Operation:
    """One operation or constructor, or one overload of it."""

    name: str
    arguments: tuple[Argument, ...]
    return_type: IdlType
    is_static: bool
    extended_attributes: ExtendedAttributes
    condition: Condition


@dataclasses.dataclass(frozen=True)
class OperationGroup:
    """The overloads of one operation, or the constructors of an interface, which make one function.

    Attributes:
        length: the function's length, the fewest arguments any overload needs.
        extended_attributes: those that Blink copies from the overloads to the group.
    """

    name: str
    overloads: tuple[Operation, ...]
    is_static: bool
    length: int
    extended_attributes: ExtendedAttributes
    condition: Condition

    def only_overload(self, where: str) -> Operation:
        if len(self.overloads) != 1:
            raise UnhandledError(f"{where}: the generator does not handle overloads of {self.name}()")
        return self.overloads[0]


@dataclasses.dataclass(frozen=True)
class Attribute:
    name: str
    idl_type: IdlType
    is_readonly: bool
    is_static: bool
    extended_attributes: ExtendedAttributes
    condition: Condition


@dataclasses.dataclass(frozen=True)
class Constant:
    name: str
    idl_type: IdlType
    value: int | float | bool
    extended_attributes: ExtendedAttributes
    condition: Condition


@dataclasses.dataclass(frozen=True)
class CollectionLike:
    """An iterable, async iterable, maplike or setlike declaration, with the members it adds."""

    kind: str
    """"iterable", "async iterable", "maplike" or "setlike"."""
    attributes: tuple[Attribute, ...]
    operation_groups: tuple[OperationGroup, ...]
    """The operations that the interface does not define itself, in Blink's order."""
    iterator_operation: str | None
    """The operation that @@iterator or @@asyncIterator is, or None for a value iterator."""


@dataclasses.dataclass(frozen=True)
class ExposedConstruct:
    """A property of a global object that gives a construct exposed on it, as bind_gen's exposed construct entries.

    Attributes:
        name: the property's name.
        kind: "interface", "callback interface" or "namespace" for the construct's own object, "alias" for a
            [LegacyWindowAlias] of an interface object, or "factory function" for a [LegacyFactoryFunction].
        target: the interface or namespace whose object, or whose factory function, the property gives.
        condition: when it is exposed, as the global object sees it.
        length: a factory function's length.
    """

    name: str
    kind: str
    target: str
    condition: Condition
    length: int = 0


@dataclasses.dataclass(frozen=True)
class Interface:
    """An interface, a namespace or a callback interface, with its partials and mixins merged in.

    Attributes:
        kind: "interface", "namespace", "callback interface", "sync iterator" or "async iterator".
        tag: the type tag that Blink gives the C++ objects of this interface, which subclasses' tags follow up to
            max_subclass_tag.
        exposure: the global names it is exposed on, each with a feature that must be on or None.
        condition: when it is exposed beyond that.
        stringifier: the stringifier's attribute name, "" for an operation, or None.
        exposed_constructs: the interfaces and namespaces exposed on this global interface, in Blink's order.
        exposed: for a [Global] interface, the properties of its global object that give the constructs exposed on it,
            in the order that bind_gen lists them: the interfaces, callback interfaces and namespaces that have an
            object, then the aliases, then the factory functions.
    """

    kind: str
    name: str
    parent: str | None
    tag: int
    max_subclass_tag: int
    exposure: tuple[tuple[str, str | None], ...]
    condition: Condition
    extended_attributes: ExtendedAttributes
    attributes: tuple[Attribute, ...]
    constants: tuple[Constant, ...]
    constructor_group: OperationGroup | None
    legacy_factory_functions: tuple[OperationGroup, ...]
    operation_groups: tuple[OperationGroup, ...]
    stringifier: str | None
    collection: CollectionLike | None
    special_operations: tuple[str, ...]
    """The indexed and named property operations, such as "indexed getter", which V8 runs as interceptors."""
    legacy_window_aliases: tuple[str, ...]
    exposed_constructs: tuple[str, ...]
    unscopables: tuple[str, ...]
    """The names in the prototype's @@unscopables, its own and inherited, sorted."""
    stringifier_condition: Condition = ALWAYS
    exposed: tuple[ExposedConstruct, ...] = ()

    @property
    def has_interface_object(self) -> bool:
        """Whether a global object it is exposed on gets an object for it: a callback interface only with constants."""
        if "LegacyNoInterfaceObject" in self.extended_attributes:
            return False
        return self.kind != "callback interface" or bool(self.constants)

    def is_exposed_on(self, global_name: str) -> bool:
        return any(name in (global_name, "*") for name, _ in self.exposure)


@dataclasses.dataclass(frozen=True)
class CallbackInterface:
    name: str
    operation: str
    """The one operation that a page's object implements, such as "handleEvent"."""


@dataclasses.dataclass(frozen=True)
class DictionaryMember:
    name: str
    idl_type: IdlType
    is_required: bool
    default: str | None
    extended_attributes: ExtendedAttributes
    condition: Condition


@dataclasses.dataclass(frozen=True)
class Dictionary:
    """A dictionary. Its members are read in this order: the inherited ones first, then each level's own by name."""

    name: str
    parent: str | None
    own_members: tuple[DictionaryMember, ...]
    extended_attributes: ExtendedAttributes
