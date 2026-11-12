// The members of an interface, namespace or callback interface that has no C++
// class in the Runtime, as data, and the functions that install them.
//
// The bindings generator writes, for each such construct, a table of its own
// properties in the order of its install plan, as it writes the install
// functions of an interface that has a class. These functions install a table
// on the construct's template and, for what each context decides, on that
// context's objects, as bind_gen's install functions do, so that a page sees
// the same properties, attributes, names and lengths in the same order. Every
// attribute, operation and constructor of such a construct is a Known Gap,
// whose callbacks are those of known_gap.h, with the member's entry as their
// data.
//
// Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py
// Upstream: third_party/blink/renderer/platform/bindings/idl_member_installer.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/generated_code_helper.cc

#ifndef MITTO_BINDINGS_INTERFACE_TABLE_H_
#define MITTO_BINDINGS_INTERFACE_TABLE_H_

#include <stdint.h>

#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "mitto/bindings/idl_type_info.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-forward.h"

namespace mitto::bindings {

class PerIsolateData;

// The type tags of the pointers that the bindings put into a v8::External, so
// that one of a kind never reads as another.
inline constexpr v8::ExternalPointerTypeTag kMemberInfoTag = 1;
inline constexpr v8::ExternalPointerTypeTag kExposedConstructInfoTag = 2;
inline constexpr v8::ExternalPointerTypeTag kWrapperTypeInfoTag = 3;

// Whether a Page installs a member. Null for a member that it always installs.
using ConditionFunction = bool (*)(const PerIsolateData& data);

enum class MemberKind : uint8_t {
  kAttribute,
  kOperation,
  kConstant,
  // A data property whose value is one of V8's intrinsics, `value`, such as
  // %Array.prototype.values% for @@iterator of an interface with an indexed
  // getter.
  kIntrinsic,
  // @@iterator or @@asyncIterator of an iterable declaration: the same function
  // as the prototype's operation `forward`, such as entries().
  kIteratorAlias,
  // @@unscopables, an object of the table's `unscopables`.
  kUnscopables,
};

enum class MemberTarget : uint8_t {
  kInterface,
  kPrototype,
  kInstance,
};

// MemberInfo::flags.
inline constexpr uint16_t kMemberHasSetter = 1 << 0;
// [LegacyLenientThis]: a receiver of another kind gets undefined, without V8's
// signature check.
inline constexpr uint16_t kMemberLenientThis = 1 << 1;
// An operation that returns a promise, which a receiver of another kind
// rejects, without V8's signature check.
inline constexpr uint16_t kMemberReturnsPromise = 1 << 2;
// A static member, on the interface object, whose receiver is not checked.
inline constexpr uint16_t kMemberStatic = 1 << 3;
// [Replaceable]: the setter shadows the attribute with a data property of the
// receiver.
inline constexpr uint16_t kMemberReplaceable = 1 << 4;
// [LegacyLenientSetter]: the setter does nothing.
inline constexpr uint16_t kMemberLenientSetter = 1 << 5;
// [PutForwards]: the setter sets the attribute `forward` of what the getter
// gives.
inline constexpr uint16_t kMemberPutForwards = 1 << 6;
// [NewObject]: the getter gives a new object each time.
inline constexpr uint16_t kMemberNewObject = 1 << 7;
// An event handler attribute, which is not a Known Gap: its setter stores any
// object and turns anything else into null, and its getter gives what it
// stored, as Chrome's do.
inline constexpr uint16_t kMemberEventHandler = 1 << 8;
// A Known Gap whose default would be unsafe or impossible, which keeps
// today's error (known_gaps.py in the generator says which).
inline constexpr uint16_t kMemberRefused = 1 << 9;
// An iterator's next() and an async iterator's return(), of an iterator over
// nothing, which is done at once. They are not Known Gaps.
inline constexpr uint16_t kMemberIteratorNext = 1 << 10;
inline constexpr uint16_t kMemberIteratorReturn = 1 << 11;

// One own property of a construct's interface object, prototype or instances.
struct MemberInfo {
  // Its name, or for a symbol its well-known name, such as "@@iterator".
  const char* name;
  // The construct it belongs to.
  // RAW_PTR_EXCLUSION: #global-scope, as in WrapperTypeInfo.
  RAW_PTR_EXCLUSION const WrapperTypeInfo* owner;
  ConditionFunction condition;
  // For kIteratorAlias, the operation whose function it is. With
  // kMemberPutForwards, the attribute that the setter sets.
  const char* forward;
  // For kAttribute, its type.
  RAW_PTR_EXCLUSION const IdlTypeInfo* type;
  // For kOperation, its overloads.
  RAW_PTR_EXCLUSION const OperationInfo* operation;
  // For kConstant, its value. For kIntrinsic, the v8::Intrinsic.
  int64_t value;
  uint16_t flags;
  // For a Known Gap, a number of its own among all of them, by which a
  // document remembers that it reported it.
  uint16_t number;
  MemberKind kind;
  MemberTarget target;
  // The v8::PropertyAttribute of the property.
  uint8_t attributes;
  // An operation's length.
  int16_t length;
};

enum class ConstructorKind : uint8_t {
  // "Illegal constructor", as V8ObjectConstructor::IsValidConstructorMode().
  kNone,
  // A constructor of the IDL, a Known Gap.
  kKnownGap,
  // [HTMLConstructor], which throws as it does with no custom element defined.
  kHTMLConstructor,
};

// The C++ class that the bindings put behind an object of an interface without
// a class, made by a Known Gap: that of its nearest ancestor with a class,
// which must be one of these.
enum class ObjectBehind : uint8_t {
  kNone,
  kEventTarget,
  kEvent,
};

// InterfaceTable::flags.
// The prototype is on a global object's chain, so its [[Prototype]] cannot
// change.
inline constexpr uint8_t kInterfaceInGlobalPrototypeChain = 1 << 0;
// Location's instances have valueOf and @@toPrimitive of their own, and
// neither they nor its prototype can change their [[Prototype]].
inline constexpr uint8_t kInterfaceLocation = 1 << 1;
// HTMLAllCollection's instances are undetectable and can be called, as
// document.all.
inline constexpr uint8_t kInterfaceAllCollection = 1 << 2;
// [IsCodeLike]: V8 takes an instance for code in eval().
inline constexpr uint8_t kInterfaceCodeLike = 1 << 3;
// [LegacyNoInterfaceObject]: no `constructor` on the prototype.
inline constexpr uint8_t kInterfaceNoInterfaceObject = 1 << 4;
// The constructor is a Known Gap that keeps today's error, as a member with
// kMemberRefused does.
inline constexpr uint8_t kInterfaceConstructorRefused = 1 << 5;

struct InterfaceTable {
  // What goes on the template, in order.
  // RAW_PTR_EXCLUSION: #global-scope, as in WrapperTypeInfo, for these spans.
  RAW_PTR_EXCLUSION base::span<const MemberInfo> template_members;
  // What goes on each context's objects once it has made them, in order.
  RAW_PTR_EXCLUSION base::span<const MemberInfo> context_members;
  // The names in @@unscopables.
  RAW_PTR_EXCLUSION base::span<const char* const> unscopables;
  // When the constructor is there, null for always.
  ConditionFunction constructor_condition;
  // The constructor's overloads.
  RAW_PTR_EXCLUSION const OperationInfo* constructor_operation;
  ConstructorKind constructor;
  ObjectBehind behind;
  // For an iterator, the v8::Intrinsic its prototype inherits from, such as
  // %IteratorPrototype%, and -1 for any other.
  int16_t parent_intrinsic;
  int16_t constructor_length;
  // The constructor's number as a Known Gap.
  uint16_t constructor_number;
  uint8_t flags;
};

// The data of a member's callbacks: a v8::External of its MemberInfo.
v8::Local<v8::External> DataOfMember(v8::Isolate* isolate,
                                     const MemberInfo& member);

// WrapperTypeInfo::install_interface_template of a construct with a table.
void InstallTemplateFromTable(v8::Isolate* isolate,
                              v8::Local<v8::Template> construct_template,
                              const WrapperTypeInfo& info);

// WrapperTypeInfo::install_context_dependent_properties of a construct with a
// table.
void InstallContextPropertiesFromTable(v8::Local<v8::Context> context,
                                       v8::Local<v8::Object> interface_object,
                                       v8::Local<v8::Object> prototype_object,
                                       v8::Local<v8::Object> instance_object,
                                       const WrapperTypeInfo& info);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_INTERFACE_TABLE_H_
