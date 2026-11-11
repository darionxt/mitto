"""Writes the C++ of the bindings for the interfaces that have a C++ class.

For each such interface it writes v8_<name>.h and .cc: a callback for each attribute, operation and constructor, which
checks its receiver and arguments, converts them and calls the class, and a function that installs them on V8's
templates in the order of the install plan. It also writes the dictionaries and unions those use, the table of
runtime-enabled features and the function that puts the interface objects on a Page's global object.

What the callbacks check, and in which order, follows bind_gen's interface.py: the receiver, through V8's signature
check, whose message is "Illegal invocation"; then for a constructor that it is called with `new`; then the number of
arguments; then each argument in turn. Each failure throws the message that Blink's ExceptionMessages has for it, in
the context that ExceptionState adds, such as "Failed to execute 'x' on 'Y': ".
"""

# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/dictionary.py
# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/union.py

import dataclasses
from collections.abc import Iterable

from bindings_generator.cpp_names import file_name, function_name, member_name
from bindings_generator.cpp_types import Site, TypeMapper, header_of
from bindings_generator.errors import UnhandledError
from bindings_generator.extended_attributes import KNOWN, Role
from bindings_generator.install_plan import Phase, Property, plan_interface
from bindings_generator.model import (
    Argument,
    Attribute,
    Condition,
    Constant,
    Dictionary,
    ExtendedAttributes,
    Interface,
    OperationGroup,
    TypeKind,
)
from bindings_generator.runtime_features import Feature
from bindings_generator.web_idl_reader import PAGE_GLOBAL_NAME, Definitions

__all__ = ["WRITTEN", "ImplementedInterface", "write_bindings"]

# The extended attributes beyond those of role NONE that the writer follows on an interface it writes. Serializable and
# Transferable only matter to structured cloning, which nothing in a Page can reach yet: it has neither postMessage()
# nor structuredClone().
WRITTEN = frozenset(
    {
        "CachedAttribute",
        "Exposed",
        "LegacyUnforgeable",
        "RaisesException",
        "RuntimeEnabled",
        "Serializable",
        "Transferable",
    }
)

_BANNER = "// Written by the bindings generator in native/bindings from Blink's Web IDL\n// database. Do not edit.\n"


@dataclasses.dataclass(frozen=True)
class ImplementedInterface:
    """An interface that has a C++ class in the Runtime.

    Attributes:
        name: the interface's name, whose class is mitto::<name>.
        header: the header that declares the class.
        missing: the members whose behaviour the Runtime does not have yet, Known Gaps. Their bindings are installed as
            Chrome's are, and throw when called.
    """

    name: str
    header: str
    missing: tuple[str, ...] = ()


def _check_written(where: str, extended: ExtendedAttributes) -> None:
    for key, _ in extended.entries:
        role = KNOWN[key][0]
        if role is not Role.NONE and key not in WRITTEN:
            raise UnhandledError(f"{where}: the C++ writer does not handle the extended attribute [{key}]")


def _check_condition(where: str, condition: Condition) -> None:
    if condition.other:
        raise UnhandledError(f"{where}: the C++ writer does not handle the condition {condition.describe()}")


def _features_test(condition: Condition) -> str:
    return " && ".join(f"features.IsEnabled(RuntimeFeature::k{feature})" for feature in condition.features)


def _quote(text: str) -> str:
    return '"' + text + '"'


class _InterfaceWriter:
    def __init__(self, interface: Interface, implemented: ImplementedInterface, types: TypeMapper) -> None:
        self.interface = interface
        self.implemented = implemented
        self.types = types
        self.cls = f"mitto::{interface.name}"
        self.v8_class = f"V8{interface.name}"
        self.callbacks: list[str] = []

    def where(self, member: str) -> str:
        return f"{self.interface.name}.{member}"

    # The callbacks.

    def _receiver(self) -> list[str]:
        return [
            f"{self.cls}* const impl = ReceiverOf<{self.cls}>(info, {self.v8_class}::kWrapperTypeInfo);",
            "if (!impl) {",
            "  return;",
            "}",
        ]

    @staticmethod
    def _exception_state() -> str:
        return "ExceptionState exception_state(isolate);"

    def _arguments(self, arguments: tuple[Argument, ...], where: str) -> tuple[list[str], list[str]]:
        """The statements that convert the arguments, and the expressions that pass them."""
        lines: list[str] = []
        passed: list[str] = []
        required = sum(1 for argument in arguments if not argument.is_optional and not argument.is_variadic)
        if required:
            lines += [
                f"if (info.Length() < {required}) {{",
                f"  exception_state.ThrowTypeError(ExceptionMessages::NotEnoughArguments({required}, info.Length()));",
                "  return;",
                "}",
            ]
        for index, argument in enumerate(arguments):
            argument_where = f"argument {argument.name} of {where}"
            if argument.is_variadic:
                raise UnhandledError(f"{argument_where}: the C++ writer does not handle variadic arguments")
            target = f"arg{index + 1}_{member_name(argument.name)}"
            site = Site(fail="return;", argument_index=index)
            value = f"info[{index}]"
            # A dictionary converts undefined to its members' defaults, so an optional one whose default is {} converts
            # whatever it gets, as Blink's do.
            converts_undefined = argument.idl_type.kind is TypeKind.DICTIONARY and argument.default == "{}"
            if not argument.is_optional or converts_undefined:
                lines += self.types.from_v8(argument.idl_type, value, target, site, argument_where)
                passed.append(target)
                continue
            cpp_type = self.types.value_type(argument.idl_type, argument_where)
            if argument.default is not None:
                default = self.types.default_value(argument.idl_type, argument.default, argument_where)
                lines.append(f"{cpp_type} {target} = {default};")
                lines.append(f"if (!{value}->IsUndefined()) {{")
                converted = self.types.from_v8(argument.idl_type, value, f"{target}_value", site, argument_where)
                lines += ["  " + line for line in converted]
                lines.append(f"  {target} = std::move({target}_value);")
                lines.append("}")
                passed.append(target)
            else:
                lines.append(f"std::optional<{cpp_type}> {target};")
                lines.append(f"if (!{value}->IsUndefined()) {{")
                converted = self.types.from_v8(argument.idl_type, value, f"{target}_value", site, argument_where)
                lines += ["  " + line for line in converted]
                lines.append(f"  {target} = std::move({target}_value);")
                lines.append("}")
                passed.append(target)
        return lines, passed

    def _add_callback(self, name: str, body: list[str]) -> str:
        lines = [
            f"void {name}(const v8::FunctionCallbackInfo<v8::Value>& info) {{",
            "  v8::Isolate* const isolate = info.GetIsolate();",
        ]
        lines += ["  " + line if line else "" for line in body]
        lines.append("}")
        self.callbacks.append("\n".join(lines))
        return name

    def _missing(self, name: str) -> str:
        return self._add_callback(
            name,
            [
                *self._receiver(),
                self._exception_state(),
                "exception_state.ThrowTypeError(ExceptionMessages::NotImplemented());",
            ],
        )

    def attribute_getter(self, attribute: Attribute) -> str:
        where = f"attribute {self.where(attribute.name)}"
        name = f"{function_name(attribute.name)}AttributeGetCallback"
        if attribute.name in self.implemented.missing:
            return self._missing(name)
        cpp_name = function_name(attribute.extended_attributes.value_of("ImplementedAs") or attribute.name)
        return_type = self.types.return_type(attribute.idl_type, where)
        if "RaisesException" not in attribute.extended_attributes:
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
        if attribute.name in self.implemented.missing:
            return self._missing(name)
        if attribute.is_readonly:
            raise UnhandledError(f"{where}: the C++ writer does not handle setters of read-only attributes")
        cpp_name = function_name(attribute.extended_attributes.value_of("ImplementedAs") or attribute.name)
        site = Site(fail="return;")
        body = [*self._receiver(), self._exception_state()]
        body += self.types.from_v8(attribute.idl_type, "info[0]", "value", site, where)
        body.append(f"impl->Set{cpp_name}(value);")
        return self._add_callback(name, body)

    def operation(self, group: OperationGroup) -> str:
        where = f"operation {self.where(group.name)}"
        name = f"{function_name(group.name)}OperationCallback"
        if group.name in self.implemented.missing:
            return self._missing(name)
        operation = group.only_overload(where)
        if operation.is_static:
            raise UnhandledError(f"{where}: the C++ writer does not handle static operations")
        _check_written(where, operation.extended_attributes)
        cpp_name = function_name(operation.extended_attributes.value_of("ImplementedAs") or operation.name)
        body = [*self._receiver(), self._exception_state()]
        conversions, passed = self._arguments(operation.arguments, where)
        body += conversions
        raises = "RaisesException" in operation.extended_attributes
        if raises:
            passed.append("exception_state")
        return_type = self.types.return_type(operation.return_type, f"{where} return type")
        call = f"impl->{cpp_name}({', '.join(passed)})"
        if return_type == "void":
            body.append(f"{call};")
        else:
            body.append(f"{return_type} result = {call};")
            if raises:
                body += ["if (exception_state.HadException()) {", "  return;", "}"]
            body.append(f"info.GetReturnValue().Set({self.types.to_v8(operation.return_type, 'result')});")
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
            body = ['ThrowTypeError(isolate, "Illegal constructor");']
            return self._add_callback(name, body)
        where = f"constructor of {self.interface.name}"
        operation = group.only_overload(where)
        _check_written(where, operation.extended_attributes)
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

    # The install function.

    def _install(self, property_: Property) -> list[str]:
        target = f"{property_.target}_template"
        member = property_.member
        if property_.name == "@@toStringTag":
            return []  # SetupInterfaceTemplate() installs it.
        if property_.name == "constructor":
            return []  # V8 adds it.
        if isinstance(member, Attribute):
            where = f"attribute {self.where(member.name)}"
            _check_written(where, member.extended_attributes)
            getter = self.attribute_getter(member)
            setter = self.attribute_setter(member) if property_.setter else "nullptr"
            attributes = self._attributes(property_)
            side_effect = "v8::SideEffectType::kHasNoSideEffect"
            return [
                f"InstallAttribute(isolate, {target}, signature, {_quote(member.name)}, kInterfaceName, {getter}, "
                f"{setter}, {attributes}, {side_effect});"
            ]
        if isinstance(member, Constant):
            where = f"constant {self.where(member.name)}"
            _check_written(where, member.extended_attributes)
            if isinstance(member.value, bool) or not isinstance(member.value, int):
                raise UnhandledError(f"{where}: the C++ writer handles integer constants only")
            return [f"InstallConstant(isolate, {target}, {_quote(member.name)}, {member.value});"]
        if isinstance(member, OperationGroup):
            where = f"operation {self.where(member.name)}"
            _check_written(where, member.extended_attributes)
            callback = self.operation(member)
            return [
                f"InstallOperation(isolate, {target}, signature, {_quote(property_.name)}, kInterfaceName, {callback}, "
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

    def install_function(self) -> list[str]:
        interface = self.interface
        _check_written(f"interface {interface.name}", interface.extended_attributes)
        for unhandled in ("collection", "stringifier", "special_operations", "legacy_factory_functions"):
            if getattr(interface, unhandled):
                raise UnhandledError(f"interface {interface.name}: the C++ writer does not handle its {unhandled}")
        plan = plan_interface(interface)
        parent = "nullptr" if interface.parent is None else f"&V8{interface.parent}::kWrapperTypeInfo"
        lines = [
            "void InstallInterfaceTemplate(v8::Isolate* isolate, v8::Local<v8::FunctionTemplate> interface_template) {",
            "  [[maybe_unused]] const RuntimeFeatureSet& features = PerIsolateData::From(isolate)->features();",
            "  [[maybe_unused]] const v8::Local<v8::ObjectTemplate> instance_template =",
            "      interface_template->InstanceTemplate();",
            "  [[maybe_unused]] const v8::Local<v8::ObjectTemplate> prototype_template =",
            "      interface_template->PrototypeTemplate();",
            "  const v8::Local<v8::Signature> signature = v8::Signature::New(isolate, interface_template);",
            f"  SetUpInterfaceTemplate(isolate, interface_template, {parent}, kInterfaceName);",
        ]
        if interface.name == "DOMException":
            lines.append("  InheritFromErrorPrototype(isolate, interface_template);")
        constructor = self.constructor()
        length = interface.constructor_group.length if interface.constructor_group else 0
        lines.append(f"  InstallConstructor(interface_template, {constructor}, {length});")
        current: Condition | None = None
        for property_ in plan:
            if property_.phase is Phase.CONTEXT:
                raise UnhandledError(
                    f"{self.where(property_.name)}: the C++ writer does not install properties for each context"
                )
            member = property_.member
            if member is not None:
                _check_condition(self.where(property_.name), property_.condition)
            condition = property_.condition if property_.condition.features else None
            if condition != current:
                if current is not None:
                    lines.append("  }")
                if condition is not None:
                    lines.append(f"  if ({_features_test(condition)}) {{")
                current = condition
            indent = "    " if current is not None else "  "
            lines += [indent + line for line in self._install(property_)]
        if current is not None:
            lines.append("  }")
        lines.append("}")
        return lines

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
        install = self.install_function()
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
        source += [*install, "", "}  // namespace", ""]
        source += [
            f"const WrapperTypeInfo {self.v8_class}::kWrapperTypeInfo = {{",
            f"    {{{interface.tag}}},",
            "    kInterfaceName,",
            f"    {'nullptr' if interface.parent is None else f'&V8{interface.parent}::kWrapperTypeInfo'},",
            f"    static_cast<v8::CppHeapPointerTag>({interface.tag}),",
            f"    static_cast<v8::CppHeapPointerTag>({interface.max_subclass_tag}),",
            "    &InstallInterfaceTemplate,",
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

    header = [
        _BANNER,
        "#ifndef MITTO_BINDINGS_DICTIONARIES_H_",
        "#define MITTO_BINDINGS_DICTIONARIES_H_",
        "",
        "#include <optional>",
        "#include <string>",
        "#include <variant>",
        "",
        '#include "v8/include/v8-local-handle.h"',
        '#include "v8/include/v8-forward.h"',
        "",
        "namespace mitto::bindings {",
        "",
        "class ExceptionState;",
        "",
    ]
    source = [
        _BANNER,
        '#include "mitto/bindings/dictionaries.h"',
        "",
        '#include "mitto/bindings/exception_context.h"',
        '#include "mitto/bindings/exception_messages.h"',
        '#include "mitto/bindings/exception_state.h"',
        '#include "mitto/bindings/v8_conversions.h"',
        '#include "v8/include/v8-context.h"',
        '#include "v8/include/v8-isolate.h"',
        '#include "v8/include/v8-object.h"',
        '#include "v8/include/v8-primitive.h"',
        "",
        "namespace mitto::bindings {",
        "",
    ]
    for name in ordered:
        struct, functions = bodies[name]
        header += struct
        source += functions
    for name, union in types.used_unions.items():
        dictionary = next(member for member in union.members if member.kind is TypeKind.DICTIONARY)
        header += [
            f"// {union.syntax}",
            f"using {name} = std::variant<{dictionary.name}, bool>;",
            "",
            "// Converts `value` to the union, as Web IDL converts a value to a union. Returns",
            "// false if that threw.",
            f"bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> value, ExceptionState& exception_state, {name}* result);",
            "",
        ]
        # Web IDL's conversion to a union: undefined, null and objects go to the dictionary, and since the union has no
        # other type for them, anything else to boolean.
        source += [
            f"bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> value, ExceptionState& exception_state, {name}* result) {{",
            "  if (value->IsNullOrUndefined() || value->IsObject()) {",
            f"    {dictionary.name} dictionary;",
            "    if (!FromV8(isolate, value, exception_state, &dictionary)) {",
            "      return false;",
            "    }",
            "    *result = std::move(dictionary);",
            "    return true;",
            "  }",
            "  *result = value->BooleanValue(isolate);",
            "  return true;",
            "}",
            "",
        ]
    header += ["}  // namespace mitto::bindings", "", "#endif  // MITTO_BINDINGS_DICTIONARIES_H_", ""]
    source += ["}  // namespace mitto::bindings", ""]
    return "\n".join(header), "\n".join(source)


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
    for member in dictionary.own_members:
        member_where = f"dictionary member {dictionary.name}.{member.name}"
        _check_written(member_where, member.extended_attributes)
        _check_condition(member_where, member.condition)
        if member.condition.features:
            raise UnhandledError(f"{member_where}: the C++ writer does not handle members behind a feature")
        if member.is_required:
            raise UnhandledError(f"{member_where}: the C++ writer does not handle required members")
        field = member_name(member.name)
        has_class = member.idl_type.kind is not TypeKind.INTERFACE or types.has_class(member.idl_type.name)
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
        fill += [
            "  {",
            f'    scope.SetCurrentPropertyName("{member.name}");',
            "    v8::Local<v8::Value> value;",
            f'    if (!object->Get(context, v8::String::NewFromUtf8Literal(isolate, "{member.name}")).ToLocal(&value)) {{',
            "      return false;",
            "    }",
            "    if (!value->IsUndefined()) {",
        ]
        site = Site(fail="return false;", dictionary=dictionary.name, member=member.name)
        converted = types.from_v8(member.idl_type, "value", "converted", site, member_where)
        fill += ["      " + line for line in converted]
        if has_class:
            fill.append(f"      dictionary->{field} = std::move(converted);")
        fill += ["    }", "  }"]
    struct += ["};", ""]
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
        "  // Whether the Engine Release has it on for stable on Windows.",
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
    """The function that puts the interface objects on a Page's global object, in the order of Window's."""
    window = definitions.interfaces[PAGE_GLOBAL_NAME]
    lines: list[str] = []
    for name in window.exposed_constructs:
        if name not in implemented:
            continue
        interface = definitions.interfaces[name]
        _check_condition(f"interface {name}", interface.condition)
        call = f'InstallInterfaceObject(context, "{name}", V8{name}::kWrapperTypeInfo);'
        if interface.condition.features:
            lines += [f"  if ({_features_test(interface.condition)}) {{", f"    {call}", "  }"]
        else:
            lines.append(f"  {call}")
    missing = set(implemented) - set(window.exposed_constructs)
    if missing:
        raise UnhandledError(f"interfaces not exposed on {PAGE_GLOBAL_NAME}: {', '.join(sorted(missing))}")
    header = [
        _BANNER,
        "#ifndef MITTO_BINDINGS_INTERFACE_OBJECTS_H_",
        "#define MITTO_BINDINGS_INTERFACE_OBJECTS_H_",
        "",
        '#include "v8/include/v8-forward.h"',
        "",
        "namespace mitto::bindings {",
        "",
        "// Puts the interface object of each interface that the Runtime implements",
        "// and the Page's features expose on the global object of `context`, in the",
        "// order of Chrome's Window.",
        "void InstallInterfaceObjects(v8::Local<v8::Context> context);",
        "",
        "}  // namespace mitto::bindings",
        "",
        "#endif  // MITTO_BINDINGS_INTERFACE_OBJECTS_H_",
        "",
    ]
    includes = sorted(header_of(name) for name in implemented)
    source = [
        _BANNER,
        '#include "mitto/bindings/interface_objects.h"',
        "",
        '#include "mitto/bindings/idl_member_installer.h"',
        '#include "mitto/bindings/per_isolate_data.h"',
        '#include "mitto/bindings/runtime_feature_set.h"',
        '#include "mitto/bindings/runtime_features.h"',
        *[f'#include "{path}"' for path in includes],
        '#include "v8/include/v8-context.h"',
        '#include "v8/include/v8-isolate.h"',
        "",
        "namespace mitto::bindings {",
        "",
        "void InstallInterfaceObjects(v8::Local<v8::Context> context) {",
        "  [[maybe_unused]] const RuntimeFeatureSet& features =",
        "      PerIsolateData::From(v8::Isolate::GetCurrent())->features();",
        *lines,
        "}",
        "",
        "}  // namespace mitto::bindings",
        "",
    ]
    return "\n".join(header), "\n".join(source)


def write_bindings(
    definitions: Definitions, features: list[Feature], implemented: Iterable[ImplementedInterface]
) -> dict[str, str]:
    """Returns each file the bindings need, by its name in the output directory.

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
        members = {m.name for m in definitions.interfaces[name].attributes}
        members |= {group.name for group in definitions.interfaces[name].operation_groups}
        for missing in interface.missing:
            if missing not in members:
                raise UnhandledError(f"{name} has no member {missing}")
    types = TypeMapper(frozenset(by_name), frozenset(definitions.dictionaries))
    files: dict[str, str] = {}
    for name, interface in sorted(by_name.items()):
        writer = _InterfaceWriter(definitions.interfaces[name], interface, types)
        header, source = writer.write()
        base = f"v8_{file_name(name)}"
        files[f"{base}.h"] = header
        files[f"{base}.cc"] = source
    files["dictionaries.h"], files["dictionaries.cc"] = _write_dictionaries(definitions, types)
    files["runtime_features.h"], files["runtime_features.cc"] = _write_features(features)
    files["interface_objects.h"], files["interface_objects.cc"] = _write_interface_objects(definitions, by_name)
    return files
