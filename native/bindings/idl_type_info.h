// Web IDL types, dictionaries and operations as data, for the members whose
// behaviour the Runtime lacks: a Known Gap converts its arguments as Chrome's
// binding does, and gives its type's default, by these tables, which the
// bindings generator writes into idl_types.cc.
//
// Upstream: third_party/blink/renderer/bindings/scripts/web_idl/idl_type.py

#ifndef MITTO_BINDINGS_IDL_TYPE_INFO_H_
#define MITTO_BINDINGS_IDL_TYPE_INFO_H_

#include <stdint.h>

#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "mitto/bindings/wrapper_type_info.h"

namespace mitto::bindings {

class PerIsolateData;

enum class IdlTypeKind : uint8_t {
  kAny,
  kUndefined,
  kBoolean,
  kByte,
  kOctet,
  kShort,
  kUnsignedShort,
  kLong,
  kUnsignedLong,
  kLongLong,
  kUnsignedLongLong,
  kFloat,
  kUnrestrictedFloat,
  kDouble,
  kUnrestrictedDouble,
  kBigInt,
  kDOMString,
  kByteString,
  kUSVString,
  kObject,
  kSymbol,
  kArrayBuffer,
  kSharedArrayBuffer,
  kArrayBufferView,
  kDataView,
  // A typed array of the kind in IdlTypeInfo::name, such as Uint8Array.
  kTypedArray,
  kInterface,
  kCallbackInterface,
  kCallbackFunction,
  kDictionary,
  kEnumeration,
  kSequence,
  kFrozenArray,
  kObservableArray,
  kRecord,
  kPromise,
  kUnion,
  // The iterator that an iterable declaration's operations return.
  kIterator,
};

// IdlTypeInfo::flags.
inline constexpr uint8_t kIdlTypeNullable = 1 << 0;
inline constexpr uint8_t kIdlTypeEnforceRange = 1 << 1;
inline constexpr uint8_t kIdlTypeClamp = 1 << 2;
inline constexpr uint8_t kIdlTypeLegacyNullToEmptyString = 1 << 3;
inline constexpr uint8_t kIdlTypeAllowShared = 1 << 4;
inline constexpr uint8_t kIdlTypeLegacyTreatNonObjectAsNull = 1 << 5;
// [BufferSourceTypeNoSizeLimit] or [PassAsSpan], which let a buffer be
// resizable or larger than usual.
inline constexpr uint8_t kIdlTypeAllowResizable = 1 << 6;

struct DictionaryInfo;

struct IdlTypeInfo {
  // The type as Blink's messages name it: "unsigned long", "Node", or for a
  // union its members' names sorted, as "(DOMString or Node)".
  const char* name;
  IdlTypeKind kind;
  uint8_t flags;
  // For kInterface, the interface, or null for one that a Page has no
  // bindings for, which no value can be an object of. For kIterator, the
  // iterator.
  // RAW_PTR_EXCLUSION: #global-scope, as in WrapperTypeInfo, for each field.
  RAW_PTR_EXCLUSION const WrapperTypeInfo* interface;
  // For kDictionary, the dictionary.
  RAW_PTR_EXCLUSION const DictionaryInfo* dictionary;
  // For kEnumeration, its strings, in order.
  RAW_PTR_EXCLUSION base::span<const char* const> values;
  // The members of a union, the element type of a sequence, frozen or
  // observable array, the key and value types of a record, or the promised
  // type of a promise.
  RAW_PTR_EXCLUSION base::span<const IdlTypeInfo* const> members;
};

struct DictionaryMemberInfo {
  const char* name;
  RAW_PTR_EXCLUSION const IdlTypeInfo* type;
  // Whether a Page reads it, null for always.
  bool (*condition)(const PerIsolateData& data);
  bool required;
  // Its default as the IDL file writes it, such as "false", "0", "\"auto\"",
  // "[]", "{}" or "null", or null for none.
  const char* default_value;
};

struct DictionaryInfo {
  const char* name;
  RAW_PTR_EXCLUSION const DictionaryInfo* parent;
  // Its own members, in the order they are read.
  RAW_PTR_EXCLUSION base::span<const DictionaryMemberInfo> members;
};

// ArgumentInfo::flags.
inline constexpr uint8_t kArgumentOptional = 1 << 0;
inline constexpr uint8_t kArgumentVariadic = 1 << 1;

struct ArgumentInfo {
  RAW_PTR_EXCLUSION const IdlTypeInfo* type;
  uint8_t flags;
};

// One overload of an operation or a constructor.
struct OverloadInfo {
  RAW_PTR_EXCLUSION base::span<const ArgumentInfo> arguments;
  RAW_PTR_EXCLUSION const IdlTypeInfo* return_type;
  // Whether a Page has it, null for always.
  bool (*condition)(const PerIsolateData& data);
};

struct OperationInfo {
  RAW_PTR_EXCLUSION base::span<const OverloadInfo> overloads;
};

// An attribute that the default toJSON steps of a Known Gap read.
struct JsonAttributeInfo {
  const char* name;
  // Whether a Page has it, null for always.
  bool (*condition)(const PerIsolateData& data);
};

// The attributes that a Known Gap's toJSON() with kMemberDefaultToJSON reads,
// in order, by its MemberInfo::value. The generator writes it in idl_types.cc.
base::span<const JsonAttributeInfo> DefaultToJSONAttributes(int64_t index);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_IDL_TYPE_INFO_H_
