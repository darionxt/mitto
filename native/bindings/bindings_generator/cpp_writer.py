"""Writes the C++ of the bindings for the interfaces that have a C++ class.

For each such interface it writes v8_<name>.h and .cc: a callback for each attribute, operation and constructor, which
checks its receiver and arguments, converts them and calls the class, a function that installs them on V8's templates
in the order of the install plan, and one that installs what each context decides on that context's objects. It also
writes the dictionaries and unions those use, the table of runtime-enabled features and the function that puts the
interface objects on a Page's global object.

What the callbacks check, and in which order, follows bind_gen's interface.py: the receiver, through V8's signature
check, whose message is "Illegal invocation"; then for a constructor that it is called with `new`; then the number of
arguments; then each argument in turn. Each failure throws the message that Blink's ExceptionMessages has for it, in
the context that ExceptionState adds, such as "Failed to execute 'x' on 'Y': ".

A member whose behaviour the Runtime does not have yet is a Known Gap: it is installed as Chrome installs it, and its
callback throws. A reflected attribute of a string or a boolean, [Reflect] without a URL or a list of values, has its
behaviour from the writer itself, which reads and writes the content attribute through the element.
"""

# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/dictionary.py
# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/union.py
# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/codegen_expr.py

import dataclasses
from collections.abc import Callable, Iterable

from bindings_generator.cpp_names import file_name, function_name, member_name
from bindings_generator.cpp_types import Site, TypeMapper, header_of, union_name
from bindings_generator.errors import UnhandledError
from bindings_generator.extended_attributes import KNOWN, Role
from bindings_generator.install_plan import Phase, Property, plan_interface
from bindings_generator.model import (
    Argument,
    Attribute,
    Condition,
    Constant,
    Dictionary,
    ExposedConstruct,
    ExtendedAttributes,
    IdlType,
    Interface,
    Operation,
    OperationGroup,
    TypeKind,
)
from bindings_generator.runtime_features import Feature
from bindings_generator.table_writer import HEADER as TABLES_HEADER
from bindings_generator.table_writer import Construct, write_tables
from bindings_generator.web_idl_reader import PAGE_GLOBAL_NAME, Definitions

__all__ = ["WRITTEN", "ImplementedInterface", "write_bindings"]

# The extended attributes beyond those of role NONE that the writer follows on a member it writes behaviour for.
# - ActiveScriptWrappable: a C++ object and its wrapper hold each other, so the wrapper and its listeners live as long as
#   anything reaches the object, as the navigator does its connection, which is all that Blink's pending activity adds
#   for the classes that have it.
# - CEReactions: the Runtime has no custom elements, so there are no reactions to run around a call.
# - CachedAccessor and CachedAttribute: a page sees the same value either way; caching only saves calls.
# - CrossOrigin: only a frame of another origin sees it, and a Page has no frames yet.
# - NewObject: the class returns a new object, which Blink only checks.
# - NoAllocDirectCall: the install plan puts the member where Blink does; V8's fast call itself only saves time.
# - SameObject: the class keeps the object and returns it on every call, so the bindings keep nothing of their own.
# - PutForwards: only an attribute's setter forwards, and the writer refuses that setter's behaviour, so an attribute
#   with it can have its getter alone.
# - Serializable and Transferable: only structured cloning reads them, which nothing in a Page can reach yet: it has
#   neither postMessage() nor structuredClone().
# The others are followed by the install plan, by the context's install function, or here. A Known Gap needs none of
# them: the install plan and its condition decide where it goes, and its callback only throws.
WRITTEN = frozenset(
    {
        "ActiveScriptWrappable",
        "CEReactions",
        "CachedAccessor",
        "CachedAttribute",
        "ContextEnabled",
        "CrossOrigin",
        "Exposed",
        "Global",
        "HTMLConstructor",
        "LegacyNoInterfaceObject",
        "LegacyUnforgeable",
        "NewObject",
        "NoAllocDirectCall",
        "NotEnumerable",
        "PutForwards",
        "RaisesException",
        "Reflect",
        "Replaceable",
        "RuntimeEnabled",
        "SameObject",
        "SecureContext",
        "Serializable",
        "Transferable",
        "Unscopable",
    }
)

# What makes a reflected attribute's behaviour more than reading and writing its content attribute.
_REFLECT_BEYOND_CONTENT = ("URL", "ReflectOnly", "ReflectMissing", "ReflectInvalid", "ReflectEmpty")

_BANNER = "// Written by the bindings generator in native/bindings from Blink's Web IDL\n// database. Do not edit.\n"


@dataclasses.dataclass(frozen=True)
class ImplementedInterface:
    """An interface that has a C++ class in the Runtime.

    Attributes:
        name: the interface's name, whose class is mitto::<name>.
        header: the header that declares the class.
        missing: the members whose behaviour the Runtime does not have yet, Known Gaps, when `implemented` is None.
            Their bindings are installed as Chrome's are, and throw when called.
        implemented: if not None, the only members whose behaviour the class has: every other member is a Known Gap,
            except a reflected attribute that the writer handles itself. "get x" names the getter of attribute x alone.
            An indexed or named property operation that is a Known Gap is not installed at all.
    """

    name: str
    header: str
    missing: tuple[str, ...] = ()
    implemented: tuple[str, ...] | None = None

    def has_behaviour(self, member: str, accessor: str | None = None) -> bool:
        """Whether the class has the member's behaviour, or for `accessor` "get" its getter's."""
        if self.implemented is None:
            return member not in self.missing
        if member in self.implemented:
            return True
        return accessor == "get" and f"get {member}" in self.implemented


def _check_written(where: str, extended: ExtendedAttributes) -> None:
    for key, _ in extended.entries:
        role = KNOWN[key][0]
        if role is not Role.NONE and key not in WRITTEN:
            raise UnhandledError(f"{where}: the C++ writer does not handle the extended attribute [{key}]")


def condition_test(condition: Condition, where: str) -> str | None:
    """The C++ test of whether a Page installs a construct, or None if it never does.

    A Page is a top-level window that is a secure context, as about:blank is, and is neither isolated nor cross-origin
    isolated, and no embedder turns a context-enabled feature on for it. A feature that a context could turn on, such
    as one an origin trial can enable, is on if the Page's set has it, since a Page has no origin trials.
    """
    if condition.alternatives:
        # The overloads of one operation whose conditions differ: Blink installs it if any of them holds.
        found = [condition_test(alternative, where) for alternative in condition.alternatives]
        alternatives = [test for test in found if test is not None]
        if not alternatives:
            return None
        if "true" in alternatives:
            return "true"
        return "(" + " || ".join(f"({test})" for test in alternatives) + ")"
    tests = [f"features.IsEnabled(RuntimeFeature::k{feature})" for feature in condition.features]
    for other in condition.other:
        if other == "SecureContext":
            tests.append("data->secure_context()")
        elif other.startswith("SecureContext="):
            names = other.removeprefix("SecureContext=").split(",")
            any_on = " || ".join(f"features.IsEnabled(RuntimeFeature::k{name})" for name in names)
            tests.append(f"(data->secure_context() || !({any_on}))")
        elif other.startswith("RuntimeEnabled=") and other.endswith(" in the context"):
            name = other.removeprefix("RuntimeEnabled=").removesuffix(" in the context")
            tests.append(f"features.IsEnabled(RuntimeFeature::k{name})")
        elif other.startswith("Exposed with "):
            names = other.removeprefix("Exposed with ").split(" or ")
            tests.append("(" + " || ".join(f"features.IsEnabled(RuntimeFeature::k{name})" for name in names) + ")")
        elif other.startswith("Exposed="):
            if PAGE_GLOBAL_NAME not in other.removeprefix("Exposed=").split(","):
                return None
        elif other.startswith("CrossOriginIsolatedOrRuntimeEnabled="):
            names = other.removeprefix("CrossOriginIsolatedOrRuntimeEnabled=").split(",")
            tests.append("(" + " || ".join(f"features.IsEnabled(RuntimeFeature::k{name})" for name in names) + ")")
        elif other in ("CrossOriginIsolated", "IsolatedContext", "InjectionMitigated") or other.startswith(
            "ContextEnabled="
        ):
            return None
        else:
            raise UnhandledError(f"{where}: the C++ writer does not handle the condition {other}")
    return " && ".join(tests) if tests else "true"


def reflected_attribute(attribute: Attribute) -> str | None:
    """The content attribute that the writer reflects for `attribute` itself, or None."""
    extended = attribute.extended_attributes
    if "Reflect" not in extended or any(key in extended for key in _REFLECT_BEYOND_CONTENT):
        return None
    idl_type = attribute.idl_type
    if idl_type.nullable or idl_type.extended_attributes.entries:
        return None
    if not ((idl_type.kind is TypeKind.STRING and idl_type.name == "DOMString") or idl_type.kind is TypeKind.BOOLEAN):
        return None
    # Blink's IDL files write a content attribute's "-" as "_", as in Reflect=http_equiv.
    return (extended.value_of("Reflect") or attribute.name.lower()).replace("_", "-")


def _quote(text: str) -> str:
    return '"' + text + '"'


def _indent(lines: list[str], prefix: str = "  ") -> list[str]:
    return [prefix + line if line else "" for line in lines]


class _InterfaceWriter:
    def __init__(
        self,
        interface: Interface,
        implemented: ImplementedInterface,
        types: TypeMapper,
        in_global_prototype_chain: bool,
        interface_object_parent: str | None,
        index: int,
        has_class: Callable[[str], bool],
    ) -> None:
        self.interface = interface
        self.implemented = implemented
        self.types = types
        self.in_global_prototype_chain = in_global_prototype_chain
        self.interface_object_parent = interface_object_parent
        self.index = index
        self.has_class = has_class
        self.constructs: list[ExposedConstruct] = []
        self.cls = f"mitto::{interface.name}"
        self.v8_class = f"V8{interface.name}"
        self.callbacks: list[str] = []
        self.callback_names: set[str] = set()

    def where(self, member: str) -> str:
        return f"{self.interface.name}.{member}"

    def method_name(self, member: Attribute | Operation) -> str:
        """The C++ method of a member: its name, or its [ImplementedAs]'s, with the first letter in upper case, and
        "ForBinding" after it when that is the class's own name, as Window's `window` would be."""
        name = function_name(member.extended_attributes.value_of("ImplementedAs") or member.name)
        return f"{name}ForBinding" if name == self.interface.name else name

    # The callbacks.

    def _receiver(self) -> list[str]:
        return [
            f"{self.cls}* const impl = ReceiverOf<{self.cls}>(info, {self.v8_class}::kWrapperTypeInfo);",
            "if (!impl) {",
            "  return;",
            "}",
        ]

    def _lenient_receiver(self) -> list[str]:
        """[LegacyLenientThis]: with no signature, a receiver of another kind gets undefined and no error."""
        return [
            f"{self.cls}* const impl = ToWrappableOf<{self.cls}>(isolate, {self.v8_class}::kWrapperTypeInfo, info.This());",
            "if (!impl) {",
            "  return;",
            "}",
        ]

    def _promise_receiver(self) -> list[str]:
        """An operation that returns a promise has no signature, and rejects for a receiver of another kind."""
        return [
            f"{self.cls}* const impl = ToWrappableOf<{self.cls}>(isolate, {self.v8_class}::kWrapperTypeInfo, info.This());",
            "if (!impl) {",
            '  RejectWithTypeError(info, "Illegal invocation");',
            "  return;",
            "}",
        ]

    @staticmethod
    def returns_promise(group: OperationGroup) -> bool:
        return any(overload.return_type.kind is TypeKind.PROMISE for overload in group.overloads)

    @staticmethod
    def _exception_state() -> str:
        return "ExceptionState exception_state(isolate);"

    def _arguments(
        self, arguments: tuple[Argument, ...], where: str, check_length: bool = True
    ) -> tuple[list[str], list[str]]:
        """The statements that convert the arguments, and the expressions that pass them."""
        lines: list[str] = []
        passed: list[str] = []
        required = sum(1 for argument in arguments if not argument.is_optional and not argument.is_variadic)
        if required and check_length:
            lines += [
                f"if (info.Length() < {required}) {{",
                f"  exception_state.ThrowTypeError(ExceptionMessages::NotEnoughArguments({required}, info.Length()));",
                "  return;",
                "}",
            ]
        for index, argument in enumerate(arguments):
            argument_where = f"argument {argument.name} of {where}"
            target = f"arg{index + 1}_{member_name(argument.name)}"
            value = f"info[{index}]"
            if argument.is_variadic:
                if index != len(arguments) - 1:
                    raise UnhandledError(f"{argument_where}: a variadic argument that is not the last")
                cpp_type = self.types.value_type(argument.idl_type, argument_where)
                site = Site(fail="return;", argument_index_expression="i")
                converted = self.types.from_v8(argument.idl_type, "info[i]", "item", site, argument_where)
                lines += [
                    f"std::vector<{cpp_type}> {target};",
                    f"for (int i = {index}; i < info.Length(); ++i) {{",
                    *_indent(converted),
                    f"  {target}.push_back(std::move(item));",
                    "}",
                ]
                passed.append(target)
                continue
            site = Site(fail="return;", argument_index=index)
            # A dictionary converts undefined to its members' defaults, so an optional one whose default is {} converts
            # whatever it gets, as Blink's do.
            reduced = self.types.reduce(argument.idl_type)
            converts_undefined = reduced.kind is TypeKind.DICTIONARY and argument.default == "{}"
            if not argument.is_optional or converts_undefined:
                lines += self.types.from_v8(argument.idl_type, value, target, site, argument_where)
                passed.append(target)
                continue
            cpp_type = self.types.value_type(argument.idl_type, argument_where)
            converted = self.types.from_v8(argument.idl_type, value, f"{target}_value", site, argument_where)
            if argument.default is not None:
                default = self.types.default_value(argument.idl_type, argument.default, argument_where)
                lines.append(f"{cpp_type} {target} = {default};")
            else:
                lines.append(f"std::optional<{cpp_type}> {target};")
            lines += [
                f"if (!{value}->IsUndefined()) {{",
                *_indent(converted),
                f"  {target} = std::move({target}_value);",
            ]
            lines.append("}")
            passed.append(target)
        return lines, passed

    def _add_callback(self, name: str, body: list[str]) -> str:
        if name in self.callback_names:
            raise UnhandledError(f"interface {self.interface.name}: two callbacks named {name}")
        self.callback_names.add(name)
        lines = [
            f"void {name}(const v8::FunctionCallbackInfo<v8::Value>& info) {{",
            # A value that the class makes, such as a frozen array, needs no isolate to return.
            "  [[maybe_unused]] v8::Isolate* const isolate = info.GetIsolate();",
        ]
        lines += _indent(body)
        lines.append("}")
        self.callbacks.append("\n".join(lines))
        return name

    def _missing(self, name: str, has_receiver: bool = True, lenient: bool = False) -> str:
        receiver = self._lenient_receiver() if lenient else self._receiver()
        return self._add_callback(
            name,
            [
                *(receiver if has_receiver else []),
                self._exception_state(),
                "exception_state.ThrowTypeError(ExceptionMessages::NotImplemented());",
            ],
        )

    def _missing_promise(self, name: str, operation: str, has_receiver: bool) -> str:
        # The error goes into the promise, so V8 adds no context to it: the message carries its own.
        message = f"Failed to execute '{operation}' on '{self.interface.name}': Not implemented."
        return self._add_callback(
            name,
            [
                *(self._promise_receiver() if has_receiver else []),
                f'RejectWithTypeError(info, "{message}");',
            ],
        )

    def _has_attribute_behaviour(self, attribute: Attribute, accessor: str | None) -> bool:
        """Whether the bindings have the behaviour of the attribute, or for `accessor` "get" its getter's."""
        if reflected_attribute(attribute) is not None:
            # The writer has it itself, unless the interface says it is missing.
            return attribute.name not in self.implemented.missing
        return self.implemented.has_behaviour(attribute.name, accessor)

    def attribute_getter(self, attribute: Attribute) -> str:
        where = f"attribute {self.where(attribute.name)}"
        name = f"{function_name(attribute.name)}AttributeGetCallback"
        reflected = reflected_attribute(attribute)
        lenient = "LegacyLenientThis" in attribute.extended_attributes
        if not self._has_attribute_behaviour(attribute, "get"):
            return self._missing(name, has_receiver=not attribute.is_static, lenient=lenient)
        _check_written(where, attribute.extended_attributes)
        if attribute.is_static:
            raise UnhandledError(f"{where}: the C++ writer does not handle static attributes")
        if reflected is not None:
            if attribute.idl_type.kind is TypeKind.BOOLEAN:
                call = f'impl->ReflectedBooleanAttribute(u"{reflected}")'
            else:
                call = f'impl->ReflectedStringAttribute(u"{reflected}")'
            return self._add_callback(
                name, [*self._receiver(), f"info.GetReturnValue().Set({self.types.to_v8(attribute.idl_type, call)});"]
            )
        cpp_name = self.method_name(attribute)
        return_type = self.types.return_type(attribute.idl_type, where)
        raises = attribute.extended_attributes.values_of("RaisesException")
        if "RaisesException" not in attribute.extended_attributes or (raises and "Getter" not in raises):
            call = f"impl->{cpp_name}()"
            return self._add_callback(
                name, [*self._receiver(), f"info.GetReturnValue().Set({self.types.to_v8(attribute.idl_type, call)});"]
            )
        return self._add_callback(
            name,
            [
                *self._receiver(),
                self._exception_state(),
                f"{return_type} result = impl->{cpp_name}(exception_state);",
                "if (exception_state.HadException()) {",
                "  return;",
                "}",
                f"info.GetReturnValue().Set({self.types.to_v8(attribute.idl_type, 'result')});",
            ],
        )

    def attribute_setter(self, attribute: Attribute) -> str:
        where = f"attribute {self.where(attribute.name)}"
        name = f"{function_name(attribute.name)}AttributeSetCallback"
        reflected = reflected_attribute(attribute)
        lenient = "LegacyLenientThis" in attribute.extended_attributes
        if not self._has_attribute_behaviour(attribute, None):
            return self._missing(name, has_receiver=not attribute.is_static, lenient=lenient)
        extended = attribute.extended_attributes
        if "Replaceable" in extended:
            # Blink's make_steps_of_replaceable(): the page's value shadows the attribute on the receiver.
            return self._add_callback(
                name,
                [
                    "bool did_create;",
                    "if (!info.This()",
                    "         ->CreateDataProperty(isolate->GetCurrentContext(),",
                    f'                              v8::String::NewFromUtf8Literal(isolate, "{attribute.name}"),',
                    "                              info[0])",
                    "         .To(&did_create)) {",
                    "  return;",
                    "}",
                ],
            )
        for key in ("PutForwards", "LegacyLenientSetter"):
            if key in extended:
                raise UnhandledError(f"{where}: the C++ writer does not handle the behaviour of [{key}]")
        if attribute.is_readonly:
            raise UnhandledError(f"{where}: the C++ writer does not handle setters of read-only attributes")
        site = Site(fail="return;")
        body = [*self._receiver(), self._exception_state()]
        body += self.types.from_v8(attribute.idl_type, "info[0]", "value", site, where)
        if reflected is not None:
            setter = (
                "SetReflectedBooleanAttribute"
                if attribute.idl_type.kind is TypeKind.BOOLEAN
                else "SetReflectedStringAttribute"
            )
            body.append(f'impl->{setter}(u"{reflected}", value);')
            return self._add_callback(name, body)
        cpp_name = self.method_name(attribute)
        raises = extended.values_of("RaisesException")
        if "RaisesException" in extended and (not raises or "Setter" in raises):
            body.append(f"impl->Set{cpp_name}(value, exception_state);")
        else:
            body.append(f"impl->Set{cpp_name}(value);")
        return self._add_callback(name, body)

    def _reachable_overloads(self, group: OperationGroup) -> list[Operation]:
        """The overloads that a call can choose: not one that needs an object of an interface without a class."""
        return [
            overload
            for overload in group.overloads
            if all(
                argument.is_optional
                or argument.is_variadic
                or argument.idl_type.nullable
                or self.types.can_hold_a_value(argument.idl_type)
                for argument in overload.arguments
            )
        ]

    def _overloads_by_type(self, overloads: list[Operation], where: str) -> tuple[int, Operation, Operation] | None:
        """For two overloads that take the same arguments but one, which is a callback function in one and a string in
        the other, as setTimeout() has: that argument's index and the two overloads, the callback function's first."""
        if len(overloads) != 2:
            return None
        first, second = overloads
        if len(first.arguments) != len(second.arguments):
            return None
        differing = [
            index
            for index, (a, b) in enumerate(zip(first.arguments, second.arguments, strict=True))
            if self.types.reduce(a.idl_type).syntax != self.types.reduce(b.idl_type).syntax
        ]
        if len(differing) != 1:
            return None
        index = differing[0]
        kinds = {self.types.reduce(overload.arguments[index].idl_type).kind for overload in overloads}
        if kinds != {TypeKind.CALLBACK_FUNCTION, TypeKind.STRING}:
            return None
        if self.types.reduce(first.arguments[index].idl_type).kind is TypeKind.CALLBACK_FUNCTION:
            return index, first, second
        return index, second, first

    def _call(self, operation: Operation, where: str, check_length: bool) -> list[str]:
        """The statements that convert an operation's arguments, call the class and set the result."""
        _check_written(where, operation.extended_attributes)
        cpp_name = self.method_name(operation)
        conversions, passed = self._arguments(operation.arguments, where, check_length)
        body = list(conversions)
        raises = "RaisesException" in operation.extended_attributes
        if raises:
            passed.append("exception_state")
        return_type = self.types.return_type(operation.return_type, f"{where} return type")
        receiver = f"{self.cls}::" if operation.is_static else "impl->"
        call = f"{receiver}{cpp_name}({', '.join(passed)})"
        if return_type == "void":
            body.append(f"{call};")
        else:
            body.append(f"{return_type} result = {call};")
            if raises:
                body += ["if (exception_state.HadException()) {", "  return;", "}"]
            body.append(f"info.GetReturnValue().Set({self.types.to_v8(operation.return_type, 'result')});")
        return body

    def operation(self, group: OperationGroup) -> str:
        where = f"operation {self.where(group.name)}"
        name = f"{function_name(group.name)}{'Static' if group.is_static else ''}OperationCallback"
        if not self.implemented.has_behaviour(group.name):
            if self.returns_promise(group):
                return self._missing_promise(name, group.name, has_receiver=not group.is_static)
            return self._missing(name, has_receiver=not group.is_static)
        body: list[str] = []
        if self.returns_promise(group):
            if len(group.overloads) != 1:
                raise UnhandledError(f"{where}: the C++ writer does not handle overloads that return a promise")
            # Web IDL: an error before the operation returns rejects the promise it returns.
            body.append("ExceptionToRejectPromiseScope reject_promise_scope(info);")
            if not group.is_static:
                body += self._promise_receiver()
        elif not group.is_static:
            body += self._receiver()
        body.append(self._exception_state())
        overloads = self._reachable_overloads(group)
        if not overloads:
            raise UnhandledError(f"{where}: no overload of {group.name}() can be called")
        if len(overloads) == 1:
            body += self._call(overloads[0], where, check_length=True)
            return self._add_callback(name, body)
        by_type = self._overloads_by_type(overloads, where)
        if by_type is not None:
            index, function_overload, other_overload = by_type
            required = min(
                sum(1 for argument in overload.arguments if not argument.is_optional and not argument.is_variadic)
                for overload in overloads
            )
            if required:
                body += [
                    f"if (info.Length() < {required}) {{",
                    f"  exception_state.ThrowTypeError(ExceptionMessages::NotEnoughArguments({required}, info.Length()));",
                    "  return;",
                    "}",
                ]
            # Web IDL's overload resolution at the distinguishing argument: a callable value chooses the callback
            # function, and anything else the string, which every value converts to.
            body += [f"if (info[{index}]->IsFunction()) {{"]
            body += _indent(self._call(function_overload, where, check_length=False))
            body += ["  return;", "}"]
            body += self._call(other_overload, where, check_length=False)
            return self._add_callback(name, body)
        # Web IDL's overload resolution, for overloads that differ in how many arguments they take: the one that takes
        # the most of those given.
        counts = [len(overload.arguments) for overload in overloads]
        if len(set(counts)) != len(counts) or any(
            argument.is_optional or argument.is_variadic for overload in overloads for argument in overload.arguments
        ):
            raise UnhandledError(f"{where}: the C++ writer does not handle these overloads of {group.name}()")
        ordered = sorted(overloads, key=lambda overload: len(overload.arguments), reverse=True)
        fewest = len(ordered[-1].arguments)
        if fewest:
            body += [
                f"if (info.Length() < {fewest}) {{",
                f"  exception_state.ThrowTypeError(ExceptionMessages::NotEnoughArguments({fewest}, info.Length()));",
                "  return;",
                "}",
            ]
        for overload in ordered[:-1]:
            body += [f"if (info.Length() >= {len(overload.arguments)}) {{"]
            body += _indent(self._call(overload, where, check_length=False))
            body += ["  return;", "}"]
        body += self._call(ordered[-1], where, check_length=False)
        return self._add_callback(name, body)

    def constructor(self) -> str:
        group = self.interface.constructor_group
        name = "ConstructorCallback"
        body = [
            "if (!info.IsConstructCall()) {",
            "  ThrowTypeError(isolate, ExceptionMessages::ConstructorCalledAsFunction());",
            "  return;",
            "}",
        ]
        if group is None:
            # V8ObjectConstructor::IsValidConstructorMode(), whatever the call.
            return self._add_callback(name, ['ThrowTypeError(isolate, "Illegal constructor");'])
        where = f"constructor of {self.interface.name}"
        operation = group.only_overload(where)
        _check_written(where, operation.extended_attributes)
        if "HTMLConstructor" in operation.extended_attributes:
            # V8HTMLConstructor::HtmlConstructor(): with no custom element defined for new.target, it throws.
            body.append('ThrowTypeError(isolate, "Illegal constructor");')
            return self._add_callback(name, body)
        body.append(self._exception_state())
        conversions, passed = self._arguments(operation.arguments, where)
        body += conversions
        if "RaisesException" in operation.extended_attributes:
            passed.append("exception_state")
        body.append(f"{self.cls}* const impl = {self.cls}::Create({', '.join(passed)});")
        if "RaisesException" in operation.extended_attributes:
            body += ["if (exception_state.HadException()) {", "  return;", "}"]
        body += ["impl->AssociateWithWrapper(isolate, info.This());", "info.GetReturnValue().Set(info.This());"]
        return self._add_callback(name, body)

    # The install functions.

    def _member_where(self, property_: Property) -> str:
        member = property_.member
        if isinstance(member, ExposedConstruct):
            return f"exposed construct {self.where(member.name)}"
        if isinstance(member, Attribute):
            return f"attribute {self.where(member.name)}"
        if isinstance(member, Constant):
            return f"constant {self.where(member.name)}"
        return f"operation {self.where(property_.name)}"

    def _check_member(self, property_: Property) -> None:
        member = property_.member
        where = self._member_where(property_)
        if isinstance(member, Attribute):
            behaviour = self._has_attribute_behaviour(member, "get")
        elif isinstance(member, OperationGroup):
            behaviour = self.implemented.has_behaviour(member.name)
        else:
            behaviour = True
        if member is not None and not isinstance(member, ExposedConstruct) and behaviour:
            _check_written(where, member.extended_attributes)

    def _install(self, property_: Property, on_objects: bool) -> list[str]:
        """The statements that install one property, on a template or, for what a context decides, on an object."""
        target = f"{property_.target}_object" if on_objects else f"{property_.target}_template"
        member = property_.member
        no_signature = property_.target == "interface"
        if isinstance(member, Attribute) and "LegacyLenientThis" in member.extended_attributes:
            no_signature = True
        if isinstance(member, OperationGroup) and self.returns_promise(member):
            no_signature = True
        signature = "v8::Local<v8::Signature>()" if no_signature else "signature"
        if property_.name == "@@toStringTag":
            return []  # SetupInterfaceTemplate() installs it.
        if property_.name == "constructor":
            return []  # V8 adds it.
        if property_.name == "@@iterator":
            collection = self.interface.collection
            if not on_objects or collection is None or collection.iterator_operation is None:
                raise UnhandledError(
                    f"{self.where(property_.name)}: the C++ writer installs @@iterator of iterables only"
                )
            # Blink's _make_install_prototype_object(): the same function as the operation it is, such as entries().
            return [
                "{",
                "  v8::Local<v8::Value> iterator_function;",
                "  if (prototype_object",
                f'          ->Get(context, v8::String::NewFromUtf8Literal(isolate, "{collection.iterator_operation}"))',
                "          .ToLocal(&iterator_function)) {",
                "    std::ignore = prototype_object->DefineOwnProperty(context, v8::Symbol::GetIterator(isolate),",
                "                                                      iterator_function, v8::DontEnum);",
                "  }",
                "}",
            ]
        if isinstance(member, ExposedConstruct):
            if member.kind == "factory function" and self.has_class(member.target):
                raise UnhandledError(
                    f"{self.where(member.name)}: the C++ writer does not call the class of a factory function"
                )
            if member not in self.constructs:
                self.constructs.append(member)
            construct = f"kExposedConstructs[{self.constructs.index(member)}]"
            if on_objects:
                return [f"InstallExposedConstructOnObject(isolate, context, instance_object, {construct});"]
            return [f"InstallExposedConstruct(isolate, instance_template, {construct});"]
        if property_.name == "@@unscopables":
            if not on_objects:
                raise UnhandledError(f"{self.where(property_.name)}: @@unscopables on a template")
            names = ", ".join(_quote(name) for name in self.interface.unscopables)
            return [
                "{",
                f"  static constexpr const char* kUnscopablePropertyNames[] = {{{names}}};",
                "  InstallUnscopablePropertyNames(isolate, context, prototype_object, kUnscopablePropertyNames);",
                "}",
            ]
        self._check_member(property_)
        if isinstance(member, Attribute):
            getter = self.attribute_getter(member)
            setter = self.attribute_setter(member) if property_.setter else "nullptr"
            attributes = self._attributes(property_)
            side_effect = "v8::SideEffectType::kHasNoSideEffect"
            install = "InstallAttributeOnObject" if on_objects else "InstallAttribute"
            first = "isolate, context" if on_objects else "isolate"
            return [
                f"{install}({first}, {target}, {signature}, {_quote(member.name)}, kInterfaceName, {getter}, "
                f"{setter}, {attributes}, {side_effect});"
            ]
        if isinstance(member, Constant):
            if on_objects:
                raise UnhandledError(
                    f"{self._member_where(property_)}: the C++ writer installs no constant per context"
                )
            if isinstance(member.value, bool) or not isinstance(member.value, int):
                raise UnhandledError(f"{self._member_where(property_)}: the C++ writer handles integer constants only")
            return [f"InstallConstant(isolate, {target}, {_quote(member.name)}, {member.value});"]
        if isinstance(member, OperationGroup):
            callback = self.operation(member)
            install = "InstallOperationOnObject" if on_objects else "InstallOperation"
            first = "isolate, context" if on_objects else "isolate"
            return [
                f"{install}({first}, {target}, {signature}, {_quote(property_.name)}, kInterfaceName, {callback}, "
                f"{property_.length}, {self._attributes(property_)}, v8::SideEffectType::kHasSideEffect);"
            ]
        raise UnhandledError(f"{self.where(property_.name)}: the C++ writer does not install this kind of property")

    @staticmethod
    def _attributes(property_: Property) -> str:
        flags: list[str] = []
        if not property_.enumerable:
            flags.append("v8::DontEnum")
        if not property_.configurable:
            flags.append("v8::DontDelete")
        if property_.writable is False and property_.kind != "value":
            flags.append("v8::ReadOnly")
        if not flags:
            return "v8::None"
        return f"static_cast<v8::PropertyAttribute>({' | '.join(flags)})"

    def _install_properties(self, properties: list[Property], on_objects: bool) -> list[str]:
        """Installs the properties in order, each under the test of its condition, with those of one test together."""
        lines: list[str] = []
        current: str | None = None
        for property_ in properties:
            test = "true"
            if property_.member is not None:
                found = condition_test(property_.condition, self.where(property_.name))
                if found is None:
                    continue
                test = found
            if property_.target == "instance" and on_objects and "Global" not in self.interface.extended_attributes:
                raise UnhandledError(
                    f"{self.where(property_.name)}: the C++ writer installs instance members per context only on a "
                    "global object"
                )
            if test != current:
                if current is not None and current != "true":
                    lines.append("}")
                if test != "true":
                    lines.append(f"if ({test}) {{")
                current = test
            statements = self._install(property_, on_objects)
            if property_.target == "instance" and on_objects:
                statements = ["if (!instance_object.IsEmpty()) {", *_indent(statements), "}"]
            lines += _indent(statements) if current != "true" else statements
        if current is not None and current != "true":
            lines.append("}")
        return lines

    def install_function(self, plan: list[Property]) -> list[str]:
        interface = self.interface
        parent = "nullptr" if interface.parent is None else f"&V8{interface.parent}::kWrapperTypeInfo"
        lines = [
            "void InstallInterfaceTemplate(v8::Isolate* isolate, v8::Local<v8::Template> construct_template,",
            "                              const WrapperTypeInfo& info) {",
            "  const v8::Local<v8::FunctionTemplate> interface_template =",
            "      construct_template.As<v8::FunctionTemplate>();",
            "  [[maybe_unused]] PerIsolateData* const data = PerIsolateData::From(isolate);",
            "  [[maybe_unused]] const RuntimeFeatureSet& features = data->features();",
            "  [[maybe_unused]] const v8::Local<v8::ObjectTemplate> instance_template =",
            "      interface_template->InstanceTemplate();",
            "  [[maybe_unused]] const v8::Local<v8::ObjectTemplate> prototype_template =",
            "      interface_template->PrototypeTemplate();",
            "  [[maybe_unused]] const v8::Local<v8::Signature> signature =",
            "      v8::Signature::New(isolate, interface_template);",
            f"  SetUpInterfaceTemplate(isolate, interface_template, {parent}, kInterfaceName);",
        ]
        if interface.name == "DOMException":
            lines.append("  InheritFromErrorPrototype(isolate, interface_template);")
        if "Global" in interface.extended_attributes:
            # Web IDL's [[SetPrototypeOf]] of a global object and of the prototypes on its chain.
            lines += ["  instance_template->SetImmutableProto();", "  prototype_template->SetImmutableProto();"]
        elif self.in_global_prototype_chain:
            lines.append("  prototype_template->SetImmutableProto();")
        constructor = self.constructor()
        if interface.constructor_group is None:
            lines.append(f"  InstallIllegalConstructor(interface_template, {constructor});")
        else:
            lines.append(
                f"  InstallConstructor(interface_template, {constructor}, {interface.constructor_group.length});"
            )
        template_properties = [property_ for property_ in plan if property_.phase is Phase.TEMPLATE]
        lines += _indent(self._install_properties(template_properties, on_objects=False))
        lines.append("}")
        return lines

    def context_function(self, plan: list[Property]) -> list[str]:
        """The function that installs what each context decides, on the objects that the context made."""
        lines = [
            "void InstallContextDependentProperties(v8::Local<v8::Context> context,",
            "                                       v8::Local<v8::Object> interface_object,",
            "                                       v8::Local<v8::Object> prototype_object,",
            "                                       v8::Local<v8::Object> instance_object,",
            "                                       const WrapperTypeInfo& info) {",
            "  [[maybe_unused]] v8::Isolate* const isolate = v8::Isolate::GetCurrent();",
            "  [[maybe_unused]] PerIsolateData* const data = PerIsolateData::From(isolate);",
            "  [[maybe_unused]] const RuntimeFeatureSet& features = data->features();",
            "  [[maybe_unused]] const v8::Local<v8::Signature> signature = v8::Signature::New(",
            f"      isolate, data->InterfaceTemplate(isolate, &{self.v8_class}::kWrapperTypeInfo));",
        ]
        if "Global" in self.interface.extended_attributes:
            # As bind_gen's per-context install functions of a [Global] interface: the global object, behind the
            # global proxy, whatever object the call names.
            lines += ["  if (instance_object.IsEmpty()) {", "    instance_object = context->Global();", "  }"]
        if "LegacyNoInterfaceObject" in self.interface.extended_attributes:
            # V8 defines `constructor` on every prototype that a template makes.
            lines.append("  DeleteConstructorProperty(isolate, context, prototype_object);")
        context_properties = [property_ for property_ in plan if property_.phase is Phase.CONTEXT]
        lines += _indent(self._install_properties(context_properties, on_objects=True))
        lines.append("}")
        return lines

    def _construct_table(self) -> list[str]:
        """The exposed constructs that the install functions name, in the order they name them."""
        if not self.constructs:
            return []
        kinds = {
            "interface": "kInterfaceObject",
            "callback interface": "kInterfaceObject",
            "alias": "kInterfaceObject",
            "namespace": "kNamespaceObject",
            "factory function": "kFactoryFunction",
        }
        lines = ["constexpr ExposedConstructInfo kExposedConstructs[] = {"]
        for construct in self.constructs:
            lines.append(
                f'    {{"{construct.name}", &V8{construct.target}::kWrapperTypeInfo, '
                f"ExposedConstructInfo::Kind::{kinds[construct.kind]}, {construct.length}}},"
            )
        return [*lines, "};", ""]

    def write(self) -> tuple[str, str]:
        interface = self.interface
        base = f"v8_{file_name(interface.name)}"
        guard = f"MITTO_BINDINGS_{base.upper()}_H_"
        header = "\n".join(
            [
                _BANNER,
                f"#ifndef {guard}",
                f"#define {guard}",
                "",
                '#include "mitto/bindings/wrapper_type_info.h"',
                '#include "v8/include/v8-forward.h"',
                "",
                "namespace mitto {",
                f"class {interface.name};",
                "}  // namespace mitto",
                "",
                "namespace mitto::bindings {",
                "",
                f"class {self.v8_class} final {{",
                " public:",
                "  static const WrapperTypeInfo kWrapperTypeInfo;",
                "",
                f"  // Returns the {interface.name} that `value` wraps, or null if it wraps none.",
                f"  static {self.cls}* ToWrappable(v8::Isolate* isolate, v8::Local<v8::Value> value);",
                "};",
                "",
                "}  // namespace mitto::bindings",
                "",
                f"#endif  // {guard}",
                "",
            ]
        )
        _check_written(f"interface {interface.name}", interface.extended_attributes)
        for unhandled in ("stringifier", "legacy_factory_functions"):
            if getattr(interface, unhandled):
                raise UnhandledError(f"interface {interface.name}: the C++ writer does not handle its {unhandled}")
        if interface.collection is not None:
            collection_members = [attribute.name for attribute in interface.collection.attributes]
            collection_members += [group.name for group in interface.collection.operation_groups]
            if any(self.implemented.has_behaviour(member) for member in collection_members):
                raise UnhandledError(
                    f"interface {interface.name}: the C++ writer does not handle the behaviour of its "
                    f"{interface.collection.kind}"
                )
        for special in interface.special_operations:
            if self.implemented.has_behaviour(special):
                raise UnhandledError(f"interface {interface.name}: the C++ writer does not handle its {special}")
        plan = plan_interface(interface)
        install = self.install_function(plan)
        context = self.context_function(plan)
        includes = {
            f"mitto/bindings/{base}.h",
            "mitto/bindings/dictionaries.h",
            "mitto/bindings/exception_messages.h",
            "mitto/bindings/exception_state.h",
            "mitto/bindings/idl_member_installer.h",
            "mitto/bindings/per_isolate_data.h",
            "mitto/bindings/runtime_feature_set.h",
            "mitto/bindings/runtime_features.h",
            "mitto/bindings/script_wrappable.h",
            "mitto/bindings/v8_conversions.h",
            self.implemented.header,
        }
        includes |= {header_of(name) for name in self.types.used_interfaces}
        includes |= {f"mitto/dom/{file_name(name)}.h" for name in self.types.used_interfaces}
        if interface.parent is not None:
            includes.add(header_of(interface.parent))
        if self.interface_object_parent is not None:
            includes.add(header_of(self.interface_object_parent))
        for construct in self.constructs:
            includes.add(
                header_of(construct.target) if self.has_class(construct.target) else f"mitto/bindings/{TABLES_HEADER}"
            )
        source = [
            _BANNER,
            f'#include "mitto/bindings/{base}.h"',
            "",
            "#include <optional>",
            "#include <string>",
            "#include <utility>",
            "#include <vector>",
            "",
        ]
        source += [f'#include "{path}"' for path in sorted(includes - {f"mitto/bindings/{base}.h"})]
        source += [
            '#include "v8/include/v8-container.h"',
            '#include "v8/include/v8-context.h"',
            '#include "v8/include/v8-exception.h"',
            '#include "v8/include/v8-function-callback.h"',
            '#include "v8/include/v8-isolate.h"',
            '#include "v8/include/v8-object.h"',
            '#include "v8/include/v8-primitive.h"',
            '#include "v8/include/v8-template.h"',
            "",
            "namespace mitto::bindings {",
            "",
            "namespace {",
            "",
            f'constexpr char kInterfaceName[] = "{interface.name}";',
            "",
        ]
        for callback in self.callbacks:
            source += [callback, ""]
        source += self._construct_table()
        source += [*install, "", *context, "", "}  // namespace", ""]
        source += [
            f"const WrapperTypeInfo {self.v8_class}::kWrapperTypeInfo = {{",
            f"    {{{interface.tag}}},",
            "    kInterfaceName,",
            f"    {'nullptr' if interface.parent is None else f'&V8{interface.parent}::kWrapperTypeInfo'},",
            f"    static_cast<v8::CppHeapPointerTag>({interface.tag}),",
            f"    static_cast<v8::CppHeapPointerTag>({interface.max_subclass_tag}),",
            "    &InstallInterfaceTemplate,",
            f"    {'nullptr' if self.interface_object_parent is None else f'&V8{self.interface_object_parent}::kWrapperTypeInfo'},",
            "    &InstallContextDependentProperties,",
            "    WrapperTypeInfo::Kind::kInterface,",
            f"    {self.index},",
            "    nullptr,",
            "};",
            "",
            f"{self.cls}* {self.v8_class}::ToWrappable(v8::Isolate* isolate, v8::Local<v8::Value> value) {{",
            f"  return ToWrappableOf<{self.cls}>(isolate, kWrapperTypeInfo, value);",
            "}",
            "",
            "}  // namespace mitto::bindings",
            "",
        ]
        return header, "\n".join(source)


def _dictionary_chain(name: str, dictionaries: dict[str, Dictionary]) -> list[Dictionary]:
    chain: list[Dictionary] = []
    current: str | None = name
    while current is not None:
        chain.insert(0, dictionaries[current])
        current = dictionaries[current].parent
    return chain


def _union_name_in_idl(union: IdlType) -> str:
    """The union as bind_gen's UnionNameInIDL() writes it for its errors: its members' names, sorted."""
    names = " or ".join(sorted(member.syntax for member in union.members))
    return f"({names}){'?' if union.nullable else ''}"


def _write_union(name: str, union: IdlType, types: TypeMapper) -> tuple[list[str], list[str]]:
    """The declaration of a union and its FromV8(), which converts a value as Web IDL converts it to a union.

    Web IDL tries the union's members in a fixed order: a dictionary for undefined and null, then an object of each
    interface, then a dictionary for any other object, then a boolean for a boolean, and then, for what is left, a
    string, or else a boolean.
    """
    alternatives = ", ".join(types.union_alternative(member) for member in union.members)
    signature = f"bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> value, ExceptionState& exception_state, {name}* result)"
    header = [
        f"// {union.syntax}",
        f"using {name} = std::variant<{alternatives}>;",
        "",
        "// Converts `value` to the union, as Web IDL converts a value to a union. Returns",
        "// false if that threw.",
        f"{signature};",
        "",
    ]
    dictionary = next((member for member in union.members if member.kind is TypeKind.DICTIONARY), None)
    interfaces = [member for member in union.members if member.kind is TypeKind.INTERFACE]
    sequence = next((member for member in union.members if member.kind is TypeKind.SEQUENCE), None)
    record = next((member for member in union.members if member.kind is TypeKind.RECORD), None)
    has_boolean = any(member.kind is TypeKind.BOOLEAN for member in union.members)
    string = next((member for member in union.members if member.kind is TypeKind.STRING), None)
    convert_dictionary = [
        f"  {dictionary.name if dictionary else ''} dictionary;",
        "  if (!FromV8(isolate, value, exception_state, &dictionary)) {",
        "    return false;",
        "  }",
        "  *result = std::move(dictionary);",
        "  return true;",
    ]
    source = [f"{signature} {{"]
    if dictionary is not None:
        source += ["  if (value->IsNullOrUndefined()) {", *_indent(convert_dictionary), "  }"]
    for interface in interfaces:
        source += [
            f"  if (mitto::{interface.name}* const object = V8{interface.name}::ToWrappable(isolate, value)) {{",
            "    *result = object;",
            "    return true;",
            "  }",
        ]
    if sequence is not None or record is not None:
        # Web IDL: an object with an @@iterator converts to the sequence, and any other object to the record.
        source.append("  if (value->IsObject()) {")
        if sequence is not None:
            element_type = types.value_type(sequence.members[0], f"union {union.syntax}")
            inner = types.from_v8(
                sequence.members[0], "item", "element", Site(fail="return false;"), f"union {union.syntax}"
            )
            source += [
                "    v8::Local<v8::Function> method;",
                "    if (!GetIteratorMethod(isolate, value.As<v8::Object>(), &method)) {",
                "      return false;",
                "    }",
                "    if (!method.IsEmpty()) {",
                f"      std::vector<{element_type}> items;",
                "      if (!IterateSequenceWithMethod(isolate, value.As<v8::Object>(), method,",
                "                                     [&](v8::Local<v8::Value> item) {",
                *["            " + line for line in inner],
                "            items.push_back(std::move(element));",
                "            return true;",
                "          })) {",
                "        return false;",
                "      }",
                "      *result = std::move(items);",
                "      return true;",
                "    }",
            ]
        if record is not None:
            converted = types.from_v8(record, "value", "entries", Site(fail="return false;"), f"union {union.syntax}")
            source += [*_indent(converted, "    "), "    *result = std::move(entries);", "    return true;"]
        source.append("  }")
    if dictionary is not None:
        source += ["  if (value->IsObject()) {", *_indent(convert_dictionary), "  }"]
    if has_boolean:
        source += [
            "  if (value->IsBoolean()) {",
            "    *result = value->BooleanValue(isolate);",
            "    return true;",
            "  }",
        ]
    if string is not None:
        cpp_string, convert = {
            "ByteString": ("std::string", "FromV8ByteString"),
            "USVString": ("std::u16string", "FromV8USVString"),
        }.get(string.name, ("std::u16string", "FromV8String"))
        source += [
            f"  {cpp_string} string;",
            f"  if (!{convert}(isolate, value, &string)) {{",
            "    return false;",
            "  }",
            "  *result = std::move(string);",
            "  return true;",
        ]
    elif has_boolean:
        source += ["  *result = value->BooleanValue(isolate);", "  return true;"]
    else:
        source += [
            f'  exception_state.ThrowTypeError(ExceptionMessages::ValueNotOfType("{_union_name_in_idl(union)}"));',
            "  return false;",
        ]
    source += ["}", ""]
    return header, source


def _write_dictionaries(definitions: Definitions, types: TypeMapper) -> tuple[str, str]:
    """dictionaries.h and .cc: a struct for each dictionary the bindings use, and a FromV8() for it and each union."""
    # Converting a dictionary may use more dictionaries, and a struct needs its parent's first.
    ordered: list[str] = []
    pending = list(types.used_dictionaries)
    bodies: dict[str, tuple[list[str], list[str]]] = {}
    while pending:
        name = pending.pop(0)
        if name in bodies:
            continue
        for ancestor in _dictionary_chain(name, definitions.dictionaries)[:-1]:
            if ancestor.name not in bodies and ancestor.name not in pending:
                pending.insert(0, ancestor.name)
        if any(a.name not in bodies for a in _dictionary_chain(name, definitions.dictionaries)[:-1]):
            pending.append(name)
            continue
        bodies[name] = _write_dictionary(definitions.dictionaries[name], types)
        ordered.append(name)
        pending += [n for n in types.used_dictionaries if n not in bodies and n not in pending]

    union_interfaces = sorted(
        {
            member.name
            for union in types.used_unions.values()
            for member in union.members
            if member.kind is TypeKind.INTERFACE
        }
    )
    header = [
        _BANNER,
        "#ifndef MITTO_BINDINGS_DICTIONARIES_H_",
        "#define MITTO_BINDINGS_DICTIONARIES_H_",
        "",
        "#include <optional>",
        "#include <string>",
        "#include <utility>",
        "#include <variant>",
        "#include <vector>",
        "",
        '#include "v8/include/v8-function.h"',
        '#include "v8/include/v8-local-handle.h"',
        '#include "v8/include/v8-forward.h"',
        "",
    ]
    if union_interfaces:
        header += ["namespace mitto {", *[f"class {name};" for name in union_interfaces], "}  // namespace mitto", ""]
    header += [
        "namespace mitto::bindings {",
        "",
        "class ExceptionState;",
        "",
    ]
    source = [
        _BANNER,
        '#include "mitto/bindings/dictionaries.h"',
        "",
        "#include <utility>",
        "",
        '#include "mitto/bindings/exception_context.h"',
        '#include "mitto/bindings/exception_messages.h"',
        '#include "mitto/bindings/exception_state.h"',
        '#include "mitto/bindings/per_isolate_data.h"',
        '#include "mitto/bindings/runtime_feature_set.h"',
        '#include "mitto/bindings/runtime_features.h"',
        '#include "mitto/bindings/v8_conversions.h"',
        *[f'#include "{header_of(name)}"' for name in union_interfaces],
        '#include "v8/include/v8-context.h"',
        '#include "v8/include/v8-isolate.h"',
        '#include "v8/include/v8-object.h"',
        '#include "v8/include/v8-primitive.h"',
        "",
        "namespace mitto::bindings {",
        "",
    ]
    union_bodies = {name: _write_union(name, union, types) for name, union in types.used_unions.items()}
    # A struct or a union needs the complete types of the dictionaries and unions it holds before it.
    for name in _declaration_order(ordered, definitions, types):
        part_header, part_source = bodies[name] if name in bodies else union_bodies[name]
        header += part_header
        source += part_source
    header += ["}  // namespace mitto::bindings", "", "#endif  // MITTO_BINDINGS_DICTIONARIES_H_", ""]
    source += ["}  // namespace mitto::bindings", ""]
    return "\n".join(header), "\n".join(source)


def _held_types(idl_type: IdlType, types: TypeMapper) -> list[str]:
    """The dictionaries and unions whose complete types a value of the type needs."""
    reduced = types.reduce(idl_type)
    if reduced.kind is TypeKind.DICTIONARY:
        return [reduced.name]
    if reduced.kind is TypeKind.UNION:
        return [union_name(reduced)]
    if reduced.kind in (TypeKind.SEQUENCE, TypeKind.RECORD):
        return [name for member in reduced.members for name in _held_types(member, types)]
    return []


def _declaration_order(dictionaries: list[str], definitions: Definitions, types: TypeMapper) -> list[str]:
    """The dictionaries and unions in an order where each comes after those it holds and its parent."""
    needs: dict[str, list[str]] = {}
    for name in dictionaries:
        dictionary = definitions.dictionaries[name]
        held = [dictionary.parent] if dictionary.parent else []
        for member in dictionary.own_members:
            if types.can_hold_a_value(member.idl_type):
                held += _held_types(member.idl_type, types)
        needs[name] = held
    for name, union in types.used_unions.items():
        needs[name] = [held for member in union.members for held in _held_types(member, types)]
    order: list[str] = []
    visiting: set[str] = set()

    def visit(name: str) -> None:
        if name in order or name not in needs:
            return
        if name in visiting:
            raise UnhandledError(f"{name} holds itself, which a C++ struct cannot")
        visiting.add(name)
        for held in needs[name]:
            visit(held)
        visiting.discard(name)
        order.append(name)

    for name in [*dictionaries, *types.used_unions]:
        visit(name)
    return order


def _write_dictionary(dictionary: Dictionary, types: TypeMapper) -> tuple[list[str], list[str]]:
    where = f"dictionary {dictionary.name}"
    _check_written(where, dictionary.extended_attributes)
    parent = f" : {dictionary.parent}" if dictionary.parent else ""
    struct = [f"struct {dictionary.name}{parent} {{"]
    fill = [
        f"bool Fill{dictionary.name}(v8::Isolate* isolate, v8::Local<v8::Object> object, ExceptionState& exception_state, "
        f"{dictionary.name}* dictionary) {{",
        "  const v8::Local<v8::Context> context = isolate->GetCurrentContext();",
    ]
    if dictionary.parent:
        fill += [
            f"  if (!Fill{dictionary.parent}(isolate, object, exception_state, dictionary)) {{",
            "    return false;",
            "  }",
        ]
    if dictionary.own_members:
        fill.append(f'  DictionaryConversionScope scope(isolate, "{dictionary.name}");')
    defaults: list[str] = []
    uses_features = False
    for member in dictionary.own_members:
        member_where = f"dictionary member {dictionary.name}.{member.name}"
        _check_written(member_where, member.extended_attributes)
        # A member behind a condition is read only where the Page has it, as Blink's conversion checks each feature.
        test = condition_test(member.condition, member_where)
        if test is None:
            continue
        field = member_name(member.name)
        has_class = types.can_hold_a_value(member.idl_type)
        if has_class:
            cpp_type = types.value_type(member.idl_type, member_where)
            if member.default is None:
                struct.append(f"  std::optional<{cpp_type}> {field};")
            elif member.idl_type.kind is TypeKind.ANY:
                struct.append(f"  {cpp_type} {field};  // Null by default.")
                defaults.append(
                    f"  dictionary->{field} = {types.default_value(member.idl_type, member.default, member_where)};"
                )
            else:
                struct.append(
                    f"  {cpp_type} {field} = {types.default_value(member.idl_type, member.default, member_where)};"
                )
        member_fill = [
            "{",
            f'  scope.SetCurrentPropertyName("{member.name}");',
            "  v8::Local<v8::Value> value;",
            f'  if (!object->Get(context, v8::String::NewFromUtf8Literal(isolate, "{member.name}")).ToLocal(&value)) {{',
            "    return false;",
            "  }",
        ]
        if member.is_required:
            member_fill += [
                "  if (value->IsUndefined()) {",
                '    exception_state.ThrowTypeError("Required member is undefined.");',
                "    return false;",
                "  }",
            ]
        member_fill.append("  if (!value->IsUndefined()) {")
        site = Site(fail="return false;", dictionary=dictionary.name, member=member.name)
        converted = types.from_v8(member.idl_type, "value", "converted", site, member_where)
        member_fill += ["    " + line for line in converted]
        if has_class:
            member_fill.append(f"    dictionary->{field} = std::move(converted);")
        member_fill += ["  }", "}"]
        if test != "true":
            member_fill = [f"if ({test}) {{", *_indent(member_fill), "}"]
            uses_features = True
        fill += _indent(member_fill)
    struct += ["};", ""]
    if uses_features:
        fill[2:2] = [
            "  [[maybe_unused]] PerIsolateData* const data = PerIsolateData::From(isolate);",
            "  [[maybe_unused]] const RuntimeFeatureSet& features = data->features();",
        ]
    fill += ["  return true;", "}", ""]
    public = [
        f"// Converts `value` to {dictionary.name}. Returns false if that threw.",
        f"bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> value, ExceptionState& exception_state, "
        f"{dictionary.name}* dictionary);",
        "",
    ]
    functions = [
        *fill,
        f"bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> value, ExceptionState& exception_state, "
        f"{dictionary.name}* dictionary) {{",
        *defaults,
        "  if (value->IsNullOrUndefined()) {",
        "    return true;",
        "  }",
        "  if (!value->IsObject()) {",
        f'    exception_state.ThrowTypeError(ExceptionMessages::ValueNotOfType("{dictionary.name}"));',
        "    return false;",
        "  }",
        f"  return Fill{dictionary.name}(isolate, value.As<v8::Object>(), exception_state, dictionary);",
        "}",
        "",
    ]
    return struct + public, ["namespace {", "", *fill, "}  // namespace", "", *functions[len(fill) :]]


def _write_features(features: list[Feature]) -> tuple[str, str]:
    index = {feature.name: position for position, feature in enumerate(features)}
    header = [
        _BANNER,
        "#ifndef MITTO_BINDINGS_RUNTIME_FEATURES_H_",
        "#define MITTO_BINDINGS_RUNTIME_FEATURES_H_",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        '#include "base/containers/span.h"',
        "",
        "namespace mitto::bindings {",
        "",
        "// Blink's runtime-enabled features, in the order of",
        "// runtime_enabled_features.json5.",
        "enum class RuntimeFeature : uint16_t {",
        *[f"  k{feature.name}," for feature in features],
        "};",
        "",
        f"inline constexpr size_t kRuntimeFeatureCount = {len(features)};",
        "",
        "struct RuntimeFeatureInfo {",
        "  const char* name;",
        "  // Whether a Page has it on by default: whether the Engine Release has it on",
        "  // for stable on Windows, on a machine without a touch screen.",
        "  bool stable;",
        "  base::span<const RuntimeFeature> depends_on;",
        "  base::span<const RuntimeFeature> implied_by;",
        "};",
        "",
        "// Every feature, by its RuntimeFeature.",
        "base::span<const RuntimeFeatureInfo> RuntimeFeatureTable();",
        "",
        "}  // namespace mitto::bindings",
        "",
        "#endif  // MITTO_BINDINGS_RUNTIME_FEATURES_H_",
        "",
    ]
    source = [
        _BANNER,
        '#include "mitto/bindings/runtime_features.h"',
        "",
        "namespace mitto::bindings {",
        "",
        "namespace {",
        "",
    ]
    for feature in features:
        for kind, names in (("DependsOn", feature.depends_on), ("ImpliedBy", feature.implied_by)):
            if names:
                items = ", ".join(f"RuntimeFeature::k{name}" for name in names)
                source.append(f"constexpr RuntimeFeature k{kind}{index[feature.name]}[] = {{{items}}};")
    source += ["", "constexpr RuntimeFeatureInfo kFeatures[] = {"]
    for feature in features:
        position = index[feature.name]
        depends = f"k{'DependsOn'}{position}" if feature.depends_on else "{}"
        implied = f"k{'ImpliedBy'}{position}" if feature.implied_by else "{}"
        stable = "true" if feature.stable else "false"
        source.append(f'    {{"{feature.name}", {stable}, {depends}, {implied}}},')
    source += [
        "};",
        "",
        "static_assert(std::size(kFeatures) == kRuntimeFeatureCount);",
        "",
        "}  // namespace",
        "",
        "base::span<const RuntimeFeatureInfo> RuntimeFeatureTable() {",
        "  return kFeatures;",
        "}",
        "",
        "}  // namespace mitto::bindings",
        "",
    ]
    return "\n".join(header), "\n".join(source).replace(
        '#include "mitto/bindings/runtime_features.h"\n',
        '#include "mitto/bindings/runtime_features.h"\n\n#include <iterator>\n',
        1,
    )


def _write_interface_objects(definitions: Definitions, implemented: dict[str, ImplementedInterface]) -> tuple[str, str]:
    """The function that sets up a Page's global object once its context exists: it makes the interface object of
    Window, which installs on the global object what the context decides, its exposed constructs among them, as Blink's
    LocalWindowProxy::InstallConditionalFeatures() asks for it."""
    if PAGE_GLOBAL_NAME not in implemented:
        raise UnhandledError(f"{PAGE_GLOBAL_NAME}, the global interface of a Page, has no C++ class")
    not_exposed = set(implemented) - set(definitions.interfaces[PAGE_GLOBAL_NAME].exposed_constructs)
    hidden = {
        name for name in not_exposed if "LegacyNoInterfaceObject" in definitions.interfaces[name].extended_attributes
    }
    if not_exposed - hidden:
        raise UnhandledError(f"interfaces not exposed on {PAGE_GLOBAL_NAME}: {', '.join(sorted(not_exposed - hidden))}")
    header = [
        _BANNER,
        "#ifndef MITTO_BINDINGS_INTERFACE_OBJECTS_H_",
        "#define MITTO_BINDINGS_INTERFACE_OBJECTS_H_",
        "",
        '#include "v8/include/v8-forward.h"',
        "",
        "namespace mitto::bindings {",
        "",
        f"// Sets up the global object of `context`, a {PAGE_GLOBAL_NAME}, as Chrome does",
        "// once a window's context exists: makes its interface object, and so",
        "// installs on the global object what the context decides, among it the",
        "// interface objects and namespaces that the context exposes. The others are",
        "// on the global object's template.",
        "void InstallInterfaceObjects(v8::Local<v8::Context> context);",
        "",
        "}  // namespace mitto::bindings",
        "",
        "#endif  // MITTO_BINDINGS_INTERFACE_OBJECTS_H_",
        "",
    ]
    source = [
        _BANNER,
        '#include "mitto/bindings/interface_objects.h"',
        "",
        "#include <tuple>",
        "",
        '#include "mitto/bindings/idl_member_installer.h"',
        f'#include "{header_of(PAGE_GLOBAL_NAME)}"',
        '#include "v8/include/v8-context.h"',
        "",
        "namespace mitto::bindings {",
        "",
        "void InstallInterfaceObjects(v8::Local<v8::Context> context) {",
        f"  std::ignore = InterfaceObject(context, V8{PAGE_GLOBAL_NAME}::kWrapperTypeInfo);",
        "}",
        "",
        "}  // namespace mitto::bindings",
        "",
    ]
    return "\n".join(header), "\n".join(source)


def _global_prototype_chains(definitions: Definitions) -> set[str]:
    """The interfaces whose prototypes are on a global object's chain: the ancestors of each [Global] interface."""
    ancestors: set[str] = set()
    for interface in definitions.interfaces.values():
        if "Global" not in interface.extended_attributes:
            continue
        parent = interface.parent
        while parent is not None:
            ancestors.add(parent)
            parent = definitions.interfaces[parent].parent
    return ancestors


def _interface_object_parent(name: str, definitions: Definitions) -> str | None:
    """The nearest ancestor that has an interface object, whose interface object is the prototype of this one's."""
    parent = definitions.interfaces[name].parent
    while parent is not None and "LegacyNoInterfaceObject" in definitions.interfaces[parent].extended_attributes:
        parent = definitions.interfaces[parent].parent
    return parent


def _check_named_members(name: str, interface: Interface, implemented: ImplementedInterface) -> None:
    attributes = {attribute.name for attribute in interface.attributes}
    members = attributes | {group.name for group in interface.operation_groups}
    members |= set(interface.special_operations)
    for missing in implemented.missing:
        if missing not in members:
            raise UnhandledError(f"{name} has no member {missing}")
    for member in implemented.implemented or ():
        if member.startswith("get "):
            if member.removeprefix("get ") not in attributes:
                raise UnhandledError(f"{name} has no attribute {member.removeprefix('get ')}")
        elif member not in members:
            raise UnhandledError(f"{name} has no member {member}")


def _constructs_with_tables(definitions: Definitions, implemented: dict[str, ImplementedInterface]) -> list[str]:
    """The constructs that get tables: each that the global object exposes, with or without an interface object, and
    each ancestor of one, that has no C++ class, sorted by name."""
    window = definitions.interfaces[PAGE_GLOBAL_NAME]
    names: set[str] = set()
    for name in window.exposed_constructs:
        interface = definitions.interfaces[name]
        if interface.kind == "callback interface" and not interface.has_interface_object:
            continue
        current: str | None = name
        while current is not None:
            names.add(current)
            current = definitions.interfaces[current].parent
    return sorted(names - set(implemented))


def write_bindings(
    definitions: Definitions,
    features: list[Feature],
    implemented: Iterable[ImplementedInterface],
    table_shards: int = 1,
) -> dict[str, str]:
    """Returns each file the bindings need, by its name in the output directory.

    Args:
        definitions: everything the reader took from the database.
        features: the runtime-enabled features.
        implemented: the interfaces that have a C++ class.
        table_shards: how many files hold the tables of the constructs that have no class.

    Raises:
        UnhandledError: if an implemented interface uses anything the writer does not handle.
    """
    by_name = {interface.name: interface for interface in implemented}
    for name, interface in by_name.items():
        if name not in definitions.interfaces or definitions.interfaces[name].kind != "interface":
            raise UnhandledError(f"{name} is not an interface of the database")
        parent = definitions.interfaces[name].parent
        if parent is not None and parent not in by_name:
            raise UnhandledError(f"{name} has a C++ class but its parent {parent} has none")
        _check_named_members(name, definitions.interfaces[name], interface)
    types = TypeMapper(frozenset(by_name), frozenset(definitions.dictionaries), definitions.enumerations)
    chains = _global_prototype_chains(definitions)
    with_tables = _constructs_with_tables(definitions, by_name)
    # Every construct with bindings has a number of its own, by name.
    indexes = {name: index for index, name in enumerate(sorted([*by_name, *with_tables]))}
    files: dict[str, str] = {}
    for name, interface in sorted(by_name.items()):
        writer = _InterfaceWriter(
            definitions.interfaces[name],
            interface,
            types,
            name in chains,
            _interface_object_parent(name, definitions),
            indexes[name],
            by_name.__contains__,
        )
        header, source = writer.write()
        base = f"v8_{file_name(name)}"
        files[f"{base}.h"] = header
        files[f"{base}.cc"] = source
    constructs = [
        Construct(
            definitions.interfaces[name], indexes[name], _interface_object_parent(name, definitions), name in chains
        )
        for name in with_tables
    ]
    files.update(write_tables(constructs, table_shards, by_name.__contains__, condition_test))
    files["dictionaries.h"], files["dictionaries.cc"] = _write_dictionaries(definitions, types)
    files["runtime_features.h"], files["runtime_features.cc"] = _write_features(features)
    files["interface_objects.h"], files["interface_objects.cc"] = _write_interface_objects(definitions, by_name)
    return files
