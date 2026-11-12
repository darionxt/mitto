"""What each member whose behaviour the Runtime lacks does: the rules of docs/runtime.md, "What a Known Gap does".

A Known Gap runs Chrome's checks and converts its arguments, tells the Session, and gives its type's default. The
generator refuses by name, so that it keeps today's error, a Known Gap whose default would be unsafe or impossible:

- a member of Crypto or SubtleCrypto, whose default would be predictable random values or keys;
- one whose default would be an object of an interface that has a C++ class, or that inherits from one, whose class's
  members would find no object behind it. An interface whose nearest ancestor with a class is EventTarget or Event
  is the exception: its objects get that class's object behind them, so that addEventListener(), dispatchEvent() and
  Event's members work on them;
- one whose default the Runtime cannot make, such as a callback that is not nullable.

Event handler attributes are not Known Gaps: their setter stores any object and turns anything else into null, and
their getter gives what it stored, which is Chrome's behaviour.
"""

# Upstream: third_party/blink/renderer/bindings/core/v8/js_event_handler.cc

from collections.abc import Callable

from bindings_generator.model import Attribute, IdlType, Interface, OperationGroup, TypeKind

__all__ = ["CLASSES_BEHIND_OBJECTS", "EVENT_HANDLER_CALLBACKS", "KnownGaps"]

# The classes that the bindings put behind an object of an interface without a class that inherits from them.
CLASSES_BEHIND_OBJECTS = frozenset({"EventTarget", "Event"})

# The callback functions of event handler attributes, as typedefs resolve EventHandler and its kin.
EVENT_HANDLER_CALLBACKS = frozenset(
    {"EventHandlerNonNull", "OnErrorEventHandlerNonNull", "OnBeforeUnloadEventHandlerNonNull"}
)

_REFUSED_OWNERS = frozenset({"Crypto", "SubtleCrypto"})


class KnownGaps:
    """Numbers the Known Gaps, and decides which of them keep today's error.

    Args:
        parents: each interface's parent, by name.
        has_class: whether an interface has a C++ class.
        has_bindings: whether an interface has bindings, a class or a table, so that the Runtime can make its objects.
    """

    def __init__(
        self,
        parents: dict[str, str | None],
        has_class: Callable[[str], bool],
        has_bindings: Callable[[str], bool],
    ) -> None:
        self._parents = parents
        self._has_class = has_class
        self._has_bindings = has_bindings
        self._attribute_numbers: dict[tuple[str, str], int] = {}
        self.count = 0

    def number(self) -> int:
        """A number of its own for the next Known Gap, by which a document remembers that it reported it."""
        self.count += 1
        return self.count - 1

    def attribute_number(self, interface: str, attribute: str) -> int:
        """The number of an attribute's Known Gap, which its accessors and a stringifier that gives it share, and
        under which an object keeps what the setter stored."""
        key = (interface, attribute)
        if key not in self._attribute_numbers:
            self._attribute_numbers[key] = self.number()
        return self._attribute_numbers[key]

    def class_behind(self, interface: str) -> str | None:
        """The nearest class among an interface and its ancestors, or None."""
        current: str | None = interface
        while current is not None:
            if self._has_class(current):
                return current
            current = self._parents.get(current)
        return None

    def object_is_safe(self, interface: str) -> bool:
        """Whether the Runtime can make an object of the interface with nothing behind it."""
        if not self._has_bindings(interface) or self._has_class(interface):
            return False
        behind = self.class_behind(interface)
        return behind is None or behind in CLASSES_BEHIND_OBJECTS

    def default_is_safe(self, idl_type: IdlType) -> bool:
        """Whether the type's default is safe and possible."""
        if idl_type.nullable:
            return True
        kind = idl_type.kind
        if kind is TypeKind.INTERFACE:
            return self.object_is_safe(idl_type.name)
        if kind is TypeKind.UNION:
            return any(self.default_is_safe(member) for member in idl_type.members)
        if kind is TypeKind.PROMISE:
            return self.default_is_safe(idl_type.members[0])
        if kind is TypeKind.ITERATOR:
            # An iterator over nothing, as the generator writes one for each iterable.
            return self._has_bindings(idl_type.name)
        return kind not in (
            TypeKind.SYMBOL,
            TypeKind.CALLBACK_FUNCTION,
            TypeKind.CALLBACK_INTERFACE,
        )

    def refuses(self, owner: Interface, member: Attribute | OperationGroup | None) -> bool:
        """Whether a Known Gap of the owner keeps today's error: an attribute, an operation, or for None the
        constructor."""
        if owner.name in _REFUSED_OWNERS:
            return True
        if member is None:
            return not self.object_is_safe(owner.name)
        if isinstance(member, Attribute):
            return not self.default_is_safe(member.idl_type)
        return not all(self.default_is_safe(overload.return_type) for overload in member.overloads)

    @staticmethod
    def is_event_handler(attribute: Attribute) -> bool:
        idl_type = attribute.idl_type
        return idl_type.kind is TypeKind.CALLBACK_FUNCTION and idl_type.name in EVENT_HANDLER_CALLBACKS
