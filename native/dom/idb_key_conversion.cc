#include "mitto/dom/idb_key_conversion.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/notreached.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_split.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_conversions.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-date.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-value.h"

namespace mitto {

namespace {

// partition_alloc::MaxAllocationSize(), the most bytes that Blink takes from
// a buffer: 2 GiB less a super page of 2 MiB.
constexpr size_t kMaxAllocationSize = (size_t{1} << 31) - (size_t{1} << 21);

// DoesExceedSizeLimit() of native_value_traits_buffer_sources.cc: throws a
// RangeError and returns true for more bytes than Blink takes.
bool ExceedsSizeLimit(size_t byte_length,
                      bindings::ExceptionState& exception_state) {
  if (byte_length <= kMaxAllocationSize) {
    return false;
  }
  exception_state.ThrowRangeError(
      "The ArrayBuffer/ArrayBufferView size exceeds the supported range.");
  return true;
}

// `length` bytes from `data`, which may be null for none.
std::string BytesOf(const void* data, size_t length) {
  if (length == 0) {
    return std::string();
  }
  return std::string(static_cast<const char*>(data), length);
}

// Blink's CreateIDBKeyFromSimpleValue(): `value`, which is not an array, as
// a key, or an invalid key for a value that is none. Each buffer goes through
// what Blink's NativeValueTraits check of it first: an ArrayBuffer and a view
// that are neither resizable nor larger than Blink takes, and a
// SharedArrayBuffer, or a view on one, that cannot grow. Returns false if
// anything threw.
bool CreateIDBKeyFromSimpleValue(v8::Isolate* isolate,
                                 v8::Local<v8::Value> value,
                                 bindings::ExceptionState& exception_state,
                                 IDBKey* key) {
  DCHECK(!value->IsArray());

  if (value->IsNumber() && !std::isnan(value.As<v8::Number>()->Value())) {
    *key = IDBKey::Number(value.As<v8::Number>()->Value());
    return true;
  }

  if (value->IsString()) {
    // A string converts as it is, which cannot throw.
    std::u16string string;
    std::ignore = bindings::FromV8String(isolate, value, &string);
    *key = IDBKey::String(std::move(string));
    return true;
  }

  if (value->IsDate() && !std::isnan(value.As<v8::Date>()->ValueOf())) {
    *key = IDBKey::Date(value.As<v8::Date>()->ValueOf());
    return true;
  }

  if (value->IsArrayBuffer()) {
    const v8::Local<v8::ArrayBuffer> buffer = value.As<v8::ArrayBuffer>();
    // NativeValueTraits<DOMArrayBuffer>. Blink's DOMArrayBuffer of a detached
    // buffer holds no contents, so it is not resizable either.
    if (!buffer->WasDetached() && buffer->IsResizableByUserJavaScript()) {
      exception_state.ThrowTypeError(
          "The provided ArrayBuffer value must not be resizable.");
      return false;
    }
    if (ExceedsSizeLimit(buffer->ByteLength(), exception_state)) {
      return false;
    }
    if (buffer->WasDetached()) {
      exception_state.ThrowDOMException("DataError",
                                        "The ArrayBuffer is detached.");
      return false;
    }
    *key = IDBKey::Binary(BytesOf(buffer->Data(), buffer->ByteLength()));
    return true;
  }

  if (value->IsSharedArrayBuffer()) {
    const v8::Local<v8::SharedArrayBuffer> buffer =
        value.As<v8::SharedArrayBuffer>();
    // NativeValueTraits<DOMSharedArrayBuffer>.
    const std::shared_ptr<v8::BackingStore> store = buffer->GetBackingStore();
    if (store->IsResizableByUserJavaScript()) {
      exception_state.ThrowTypeError(
          "The provided SharedArrayBuffer value must not be resizable.");
      return false;
    }
    if (ExceedsSizeLimit(buffer->ByteLength(), exception_state)) {
      return false;
    }
    *key = IDBKey::Binary(BytesOf(store->Data(), buffer->ByteLength()));
    return true;
  }

  if (value->IsArrayBufferView()) {
    const v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
    // NativeValueTraits<MaybeShared<DOMArrayBufferView>>. A view that V8 keeps
    // on its heap has no buffer of its own yet, and so is neither shared,
    // resizable nor detached.
    bool detached = false;
    if (view->HasBuffer()) {
      const v8::Local<v8::ArrayBuffer> buffer = view->Buffer();
      detached = buffer->WasDetached();
      if (!detached && buffer->IsResizableByUserJavaScript()) {
        exception_state.ThrowTypeError(
            "The provided ArrayBufferView value must not be resizable.");
        return false;
      }
    }
    if (ExceedsSizeLimit(view->ByteLength(), exception_state)) {
      return false;
    }
    // A view on a SharedArrayBuffer is never detached, so it comes to the
    // copy below as Blink copies it.
    if (detached) {
      exception_state.ThrowDOMException("DataError",
                                        "The viewed ArrayBuffer is detached.");
      return false;
    }
    std::string bytes(view->ByteLength(), '\0');
    if (!bytes.empty()) {
      view->CopyContents(bytes.data(), bytes.size());
    }
    *key = IDBKey::Binary(std::move(bytes));
    return true;
  }

  *key = IDBKey(IDBKeyType::kInvalid);
  return true;
}

// The categories of Unicode that ECMA-262's grammar of identifiers names, as
// Blink's idb_key_path.cc masks them.
constexpr uint32_t kUnicodeLetter = U_GC_LU_MASK | U_GC_LL_MASK | U_GC_LT_MASK |
                                    U_GC_LM_MASK | U_GC_LO_MASK | U_GC_NL_MASK;
constexpr uint32_t kUnicodeCombiningMark = U_GC_MN_MASK | U_GC_MC_MASK;
constexpr uint32_t kUnicodeDigit = U_GC_ND_MASK;
constexpr uint32_t kUnicodeConnectorPunctuation = U_GC_PC_MASK;

constexpr char16_t kZeroWidthNonJoiner = 0x200C;
constexpr char16_t kZeroWidthJoiner = 0x200D;

// Blink passes each UTF-16 code unit to Unicode's categories by itself, so a
// surrogate, of none of these categories, is in no identifier.
bool IsIdentifierStartCharacter(char16_t c) {
  return (U_GET_GC_MASK(c) & kUnicodeLetter) || c == u'$' || c == u'_';
}

bool IsIdentifierCharacter(char16_t c) {
  return (U_GET_GC_MASK(c) & (kUnicodeLetter | kUnicodeCombiningMark |
                              kUnicodeDigit | kUnicodeConnectorPunctuation)) ||
         c == u'$' || c == u'_' || c == kZeroWidthNonJoiner ||
         c == kZeroWidthJoiner;
}

bool IsIdentifier(std::u16string_view s) {
  if (s.empty() || !IsIdentifierStartCharacter(s.front())) {
    return false;
  }
  return std::ranges::all_of(s.substr(1), &IsIdentifierCharacter);
}

// Blink's ParseKeyPath(): the identifiers of `key_path`, a valid one.
std::vector<std::u16string> ParseKeyPath(std::u16string_view key_path) {
  std::optional<std::vector<std::u16string>> elements =
      IDBParseKeyPath(key_path);
  DCHECK(elements);
  return elements ? *std::move(elements) : std::vector<std::u16string>();
}

// Blink's IsImplicitProperty(): a property that a key path reads, but that a
// clone of the value does not hold as its own, so that a key cannot go there.
// Blink adds a Blob's size and type and a File's name, lastModified and
// lastModifiedDate, which no value has in the Runtime.
bool IsImplicitProperty(v8::Local<v8::Value> value, std::u16string_view name) {
  return (value->IsString() || value->IsArray()) && name == u"length";
}

// Blink's CreateIDBKeyFromValueAndKeyPath() of a string, `key_path`, a valid
// one: the key, which may be invalid, or nullopt where evaluating fails.
// Returns false if anything threw.
bool CreateIDBKeyFromValueAndKeyPathString(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    std::u16string_view key_path,
    bindings::ExceptionState& exception_state,
    std::optional<IDBKey>* key) {
  const std::vector<std::u16string> elements = ParseKeyPath(key_path);
  DCHECK(isolate->InContext());

  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  for (const std::u16string& element : elements) {
    // The special cases of the standard's key path construct, which Blink
    // reads directly, so that nothing of the page's runs.
    if (value->IsString() && element == u"length") {
      const int32_t length = value.As<v8::String>()->Length();
      value = v8::Number::New(isolate, length);
      continue;
    }
    if (value->IsArray() && element == u"length") {
      // Blink takes the length into an int32_t, so one of 2^31 or more reads
      // as a negative number.
      const int32_t length =
          static_cast<int32_t>(value.As<v8::Array>()->Length());
      value = v8::Number::New(isolate, length);
      continue;
    }

    if (!value->IsObject()) {
      key->reset();
      return true;
    }
    const v8::Local<v8::Object> object = value.As<v8::Object>();

    // Blink reads a Blob's size and type, and a File's name, lastModified and
    // lastModifiedDate, here. The Runtime has neither.

    const v8::Local<v8::String> property =
        bindings::ToV8String(isolate, element);
    bool has_own_property;
    if (!object->HasOwnProperty(context, property).To(&has_own_property)) {
      return false;
    }
    if (!has_own_property) {
      key->reset();
      return true;
    }
    if (!object->Get(context, property).ToLocal(&value)) {
      return false;
    }
  }
  IDBKey result;
  if (!CreateIDBKeyFromValue(isolate, value, exception_state, &result)) {
    return false;
  }
  *key = std::move(result);
  return true;
}

// Blink's IDBKey::IsValid() in a renderer, which takes None, a hole of a
// compound index key, as valid, where IDBKey::IsValid() does not.
bool IsValidOrHole(const IDBKey& key) {
  if (key.type() == IDBKeyType::kNone) {
    return true;
  }
  if (key.type() == IDBKeyType::kArray) {
    return std::ranges::all_of(key.array(), &IsValidOrHole);
  }
  return key.IsValid();
}

// Blink's IDBKey::ToMultiEntryArray(): the valid members of `array_key`, an
// array, each once, in the order of keys.
std::vector<IDBKey> ToMultiEntryArray(const IDBKey& array_key) {
  std::vector<IDBKey> result;
  result.reserve(array_key.array().size());
  for (const IDBKey& key : array_key.array()) {
    if (key.IsValid()) {
      result.push_back(key);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const IDBKey& a, const IDBKey& b) { return a.IsLessThan(b); });
  const auto end =
      std::unique(result.begin(), result.end(),
                  [](const IDBKey& a, const IDBKey& b) { return a.Equals(b); });
  result.erase(end, result.end());
  return result;
}

}  // namespace

bool CreateIDBKeyFromValue(v8::Isolate* isolate,
                           v8::Local<v8::Value> value,
                           bindings::ExceptionState& exception_state,
                           IDBKey* key) {
  if (!value->IsArray()) {
    return CreateIDBKeyFromSimpleValue(isolate, value, exception_state, key);
  }

  // An array being converted: its length, taken when its conversion starts,
  // as the standard says, since getters may change it, and its members so
  // far.
  struct Record {
    uint32_t length;
    IDBKey::KeyArray subkeys;
  };

  // The arrays being converted, outermost first, each with its record in
  // `stack` at the same place, so that the recursion is on the heap rather
  // than the stack. An array that holds one of them holds itself, which the
  // standard refuses.
  std::vector<Record> stack;
  v8::LocalVector<v8::Array> seen(isolate);

  {
    const v8::Local<v8::Array> array = value.As<v8::Array>();
    if (array->Length() > IDBKey::kMaximumArraySize) {
      *key = IDBKey(IDBKeyType::kInvalid);
      return true;
    }
    stack.push_back(Record{array->Length(), {}});
    seen.push_back(array);
  }

  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);

  while (true) {
    DCHECK(!stack.empty());
    DCHECK_EQ(stack.size(), seen.size());
    const v8::Local<v8::Array> array = seen.back();
    const size_t item_index = stack.back().subkeys.size();

    // Done with this array?
    if (item_index == stack.back().length) {
      IDBKey done = IDBKey::Array(std::move(stack.back().subkeys));
      seen.pop_back();
      stack.pop_back();
      if (stack.empty()) {
        *key = std::move(done);
        return true;
      }
      stack.back().subkeys.push_back(std::move(done));
      continue;
    }

    // The next item of the current array.
    const uint32_t index = static_cast<uint32_t>(item_index);
    bool has_own_property;
    if (!array->HasOwnProperty(context, index).To(&has_own_property)) {
      return false;
    }
    if (!has_own_property) {
      *key = IDBKey(IDBKeyType::kInvalid);
      return true;
    }
    v8::Local<v8::Value> item;
    if (!array->Get(context, index).ToLocal(&item)) {
      return false;
    }

    if (!item->IsArray()) {
      IDBKey subkey;
      if (!CreateIDBKeyFromSimpleValue(isolate, item, exception_state,
                                       &subkey)) {
        return false;
      }
      stack.back().subkeys.push_back(std::move(subkey));
      continue;
    }

    // A sub-array, whose conversion starts.
    const v8::Local<v8::Array> subarray = item.As<v8::Array>();
    if (std::find(seen.begin(), seen.end(), subarray) != seen.end() ||
        stack.size() >= IDBKey::kMaximumDepth ||
        subarray->Length() > IDBKey::kMaximumArraySize) {
      *key = IDBKey(IDBKeyType::kInvalid);
      return true;
    }
    stack.push_back(Record{subarray->Length(), {}});
    seen.push_back(subarray);
  }
}

bool CreateValidIDBKeyFromValue(v8::Isolate* isolate,
                                v8::Local<v8::Value> value,
                                bindings::ExceptionState& exception_state,
                                IDBKey* key) {
  if (!CreateIDBKeyFromValue(isolate, value, exception_state, key)) {
    return false;
  }
  if (!key->IsValid()) {
    exception_state.ThrowDOMException("DataError", kIDBNotValidKeyErrorMessage);
    return false;
  }
  return true;
}

v8::Local<v8::Value> IDBKeyToV8(v8::Isolate* isolate, const IDBKey& key) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  switch (key.type()) {
    case IDBKeyType::kInvalid:
    case IDBKeyType::kMin:
      NOTREACHED();
    case IDBKeyType::kNone:
      return v8::Null(isolate);
    case IDBKeyType::kNumber:
      return v8::Number::New(isolate, key.number());
    case IDBKeyType::kDate:
      return v8::Date::New(context, key.number()).ToLocalChecked();
    case IDBKeyType::kString:
      return bindings::ToV8String(isolate, key.string());
    case IDBKeyType::kBinary: {
      // The standard's conversion of a key to a value makes a new
      // ArrayBuffer.
      const std::string& bytes = key.binary();
      const v8::Local<v8::ArrayBuffer> buffer =
          v8::ArrayBuffer::New(isolate, bytes.size());
      if (!bytes.empty()) {
        // SAFETY: the new buffer holds bytes.size() bytes.
        UNSAFE_BUFFERS(
            base::span(static_cast<uint8_t*>(buffer->Data()), bytes.size()))
            .copy_from(base::as_byte_span(bytes));
      }
      return buffer;
    }
    case IDBKeyType::kArray: {
      const IDBKey::KeyArray& members = key.array();
      const v8::Local<v8::Array> array =
          v8::Array::New(isolate, base::checked_cast<int>(members.size()));
      for (size_t i = 0; i < members.size(); ++i) {
        v8::Local<v8::Value> value = IDBKeyToV8(isolate, members[i]);
        if (value.IsEmpty()) {
          value = v8::Undefined(isolate);
        }
        bool created_property;
        if (!array->CreateDataProperty(context, static_cast<uint32_t>(i), value)
                 .To(&created_property) ||
            !created_property) {
          return v8::Local<v8::Value>();
        }
      }
      return array;
    }
  }
  NOTREACHED();
}

std::optional<std::vector<std::u16string>> IDBParseKeyPath(
    std::u16string_view key_path) {
  // IDBKeyPath ::= EMPTY_STRING | identifier ('.' identifier)*
  if (key_path.empty()) {
    return std::vector<std::u16string>();
  }
  // Blink's String::Split() keeps empty pieces, which are no identifiers.
  std::vector<std::u16string> elements = base::SplitString(
      key_path, u".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  for (const std::u16string& element : elements) {
    if (!IsIdentifier(element)) {
      return std::nullopt;
    }
  }
  return elements;
}

bool IDBIsValidKeyPath(std::u16string_view key_path) {
  return IDBParseKeyPath(key_path).has_value();
}

bool IDBKeyPathIsValid(const IDBKeyPath& key_path) {
  switch (key_path.type) {
    case IDBKeyPathType::kNull:
      return false;
    case IDBKeyPathType::kString:
      return IDBIsValidKeyPath(key_path.string);
    case IDBKeyPathType::kArray:
      if (key_path.array.empty()) {
        return false;
      }
      return std::ranges::all_of(key_path.array,
                                 [](const std::u16string& element) {
                                   return IDBIsValidKeyPath(element);
                                 });
  }
  NOTREACHED();
}

IDBKeyPath IDBKeyPathFromUnion(const IDBKeyPathUnion& key_path) {
  if (const std::u16string* string = std::get_if<std::u16string>(&key_path)) {
    return IDBKeyPath(*string);
  }
  return IDBKeyPath(std::get<std::vector<std::u16string>>(key_path));
}

IDBKeyPath IDBKeyPathFromUnion(const std::optional<IDBKeyPathUnion>& key_path) {
  if (!key_path) {
    return IDBKeyPath();
  }
  return IDBKeyPathFromUnion(*key_path);
}

v8::Local<v8::Value> IDBKeyPathToV8(v8::Isolate* isolate,
                                    const IDBKeyPath& key_path) {
  switch (key_path.type) {
    case IDBKeyPathType::kNull:
      return v8::Null(isolate);
    case IDBKeyPathType::kString:
      return bindings::ToV8String(isolate, key_path.string);
    case IDBKeyPathType::kArray:
      return bindings::ToV8StringArray(isolate, key_path.array);
  }
  NOTREACHED();
}

bool CreateIDBKeyFromValueAndKeyPath(v8::Isolate* isolate,
                                     v8::Local<v8::Value> value,
                                     const IDBKeyPath& key_path,
                                     bindings::ExceptionState& exception_state,
                                     std::optional<IDBKey>* key) {
  DCHECK(!key_path.IsNull());
  const v8::HandleScope handle_scope(isolate);
  if (key_path.type == IDBKeyPathType::kArray) {
    IDBKey::KeyArray result;
    result.reserve(key_path.array.size());
    for (const std::u16string& path : key_path.array) {
      std::optional<IDBKey> subkey;
      if (!CreateIDBKeyFromValueAndKeyPathString(isolate, value, path,
                                                 exception_state, &subkey)) {
        return false;
      }
      // Evaluating one of the paths failed, and so the whole.
      if (!subkey) {
        key->reset();
        return true;
      }
      result.push_back(*std::move(subkey));
    }
    // An array key, even if members may be invalid.
    *key = IDBKey::Array(std::move(result));
    return true;
  }

  DCHECK_EQ(key_path.type, IDBKeyPathType::kString);
  return CreateIDBKeyFromValueAndKeyPathString(isolate, value, key_path.string,
                                               exception_state, key);
}

bool CreateIDBKeyFromValueAndKeyPaths(v8::Isolate* isolate,
                                      v8::Local<v8::Value> value,
                                      const IDBKeyPath& store_key_path,
                                      const IDBKeyPath& index_key_path,
                                      bindings::ExceptionState& exception_state,
                                      std::optional<IDBKey>* key) {
  DCHECK(!index_key_path.IsNull());
  const v8::HandleScope handle_scope(isolate);
  if (index_key_path.type == IDBKeyPathType::kArray) {
    const bool uses_inline_keys =
        store_key_path.type == IDBKeyPathType::kString;
    IDBKey::KeyArray result;
    result.reserve(index_key_path.array.size());
    for (const std::u16string& path : index_key_path.array) {
      std::optional<IDBKey> subkey;
      if (!CreateIDBKeyFromValueAndKeyPathString(isolate, value, path,
                                                 exception_state, &subkey)) {
        return false;
      }
      if (!subkey && uses_inline_keys && store_key_path.string == path) {
        // A compound key that holds the store's in-line primary key, which
        // the key generator makes later, has a hole there.
        subkey = IDBKey();
      } else if (!subkey) {
        // Evaluating the key path failed.
        key->reset();
        return true;
      } else if (!subkey->IsValid()) {
        // Only a multiEntry index takes an array with invalid members, and an
        // array key path cannot be multiEntry.
        *key = IDBKey(IDBKeyType::kInvalid);
        return true;
      }
      result.push_back(*std::move(subkey));
    }
    *key = IDBKey::Array(std::move(result));
    return true;
  }

  DCHECK_EQ(index_key_path.type, IDBKeyPathType::kString);
  return CreateIDBKeyFromValueAndKeyPathString(
      isolate, value, index_key_path.string, exception_state, key);
}

IndexedDbIndexKeys GenerateIndexKeysForValue(
    v8::Isolate* isolate,
    const IDBKeyPath& store_key_path,
    const IndexedDbIndexMetadata& index_metadata,
    v8::Local<v8::Value> value) {
  IndexedDbIndexKeys index_keys;
  index_keys.index_id = index_metadata.id;

  // What Chrome's NonThrowableExceptionState drops, this catches and drops.
  const v8::TryCatch try_catch(isolate);
  bindings::ExceptionState exception_state(isolate);
  std::optional<IDBKey> index_key;
  if (!CreateIDBKeyFromValueAndKeyPaths(isolate, value, store_key_path,
                                        index_metadata.key_path,
                                        exception_state, &index_key)) {
    return index_keys;
  }

  // No match. For a store with a key generator and in-line keys whose key
  // path is the index's, the Session makes the index key.
  if (!index_key) {
    return index_keys;
  }

  // A multiEntry index has an entry for each member of an array, with
  // duplicates and invalid members left out, as the standard says.
  if (index_metadata.multi_entry && index_key->type() == IDBKeyType::kArray) {
    index_keys.keys = ToMultiEntryArray(*index_key);
    return index_keys;
  }

  // Otherwise, an invalid index key is simply ignored.
  if (!IsValidOrHole(*index_key)) {
    return index_keys;
  }

  // And a single key goes in the index for the record.
  index_keys.keys.push_back(*std::move(index_key));
  return index_keys;
}

bool CanInjectIDBKeyIntoScriptValue(v8::Isolate* isolate,
                                    v8::Local<v8::Value> value,
                                    const IDBKeyPath& key_path) {
  DCHECK_EQ(key_path.type, IDBKeyPathType::kString);
  const std::vector<std::u16string> elements = ParseKeyPath(key_path.string);
  if (elements.empty()) {
    return false;
  }

  v8::Local<v8::Value> current = value;
  if (!current->IsObject()) {
    return false;
  }

  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  for (const std::u16string& element : elements) {
    // A string's or an array's length cannot be overwritten.
    if (IsImplicitProperty(current, element)) {
      return false;
    }
    // Nor can a value that is not an object take a property.
    if (!current->IsObject()) {
      return false;
    }
    const v8::Local<v8::Object> object = current.As<v8::Object>();
    const v8::Local<v8::String> property =
        bindings::ToV8String(isolate, element);
    // A value without the own property can take it, as an object on the way
    // or as the key itself.
    bool has_own_property;
    if (!object->HasOwnProperty(context, property).To(&has_own_property)) {
      return false;
    }
    if (!has_own_property) {
      return true;
    }
    // Otherwise, the path goes on through it.
    if (!object->Get(context, property).ToLocal(&current)) {
      return false;
    }
  }
  return true;
}

bool InjectV8KeyIntoV8Value(v8::Isolate* isolate,
                            v8::Local<v8::Value> key,
                            v8::Local<v8::Value> value,
                            const IDBKeyPath& key_path) {
  DCHECK(isolate->InContext());
  DCHECK_EQ(key_path.type, IDBKeyPathType::kString);
  const std::vector<std::u16string> elements = ParseKeyPath(key_path.string);

  // The standard forbids a key generator with the empty key path.
  if (elements.empty()) {
    NOTREACHED();
  }

  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();

  // For a key path a.b.c, an object {} becomes {a: {b: {}}} before the key
  // goes in. The value was deserialized, so it has no getters or setters of
  // its own, though its prototypes may. Blink does not assume that it suits
  // the path: a store whose values went wrong, or were written by an older
  // format, may hold {a: {b: 42}}, or a number.
  for (size_t i = 0; i + 1 < elements.size(); ++i) {
    if (!value->IsObject()) {
      return false;
    }
    const std::u16string& element = elements[i];
    DCHECK(!IsImplicitProperty(value, element));
    const v8::Local<v8::Object> object = value.As<v8::Object>();
    const v8::Local<v8::String> property =
        bindings::ToV8String(isolate, element);
    bool has_own_property;
    if (!object->HasOwnProperty(context, property).To(&has_own_property)) {
      return false;
    }
    if (has_own_property) {
      if (!object->Get(context, property).ToLocal(&value)) {
        return false;
      }
    } else {
      value = v8::Object::New(isolate);
      bool created_property;
      if (!object->CreateDataProperty(context, property, value)
               .To(&created_property) ||
          !created_property) {
        return false;
      }
    }
  }

  // A string's or an array's length needs no key put in: the caller may check
  // that the value matches its key instead.
  if (IsImplicitProperty(value, elements.back())) {
    return true;
  }

  // Otherwise the value must be an object, which a stored value that went
  // wrong may not be.
  if (!value->IsObject()) {
    return false;
  }

  const v8::Local<v8::Object> object = value.As<v8::Object>();
  const v8::Local<v8::String> property =
      bindings::ToV8String(isolate, elements.back());
  bool created_property;
  if (!object->CreateDataProperty(context, property, key)
           .To(&created_property)) {
    return false;
  }
  return created_property;
}

int16_t CompareValuesAsIDBKeys(v8::Isolate* isolate,
                               v8::Local<v8::Value> first,
                               v8::Local<v8::Value> second,
                               bindings::ExceptionState& exception_state) {
  IDBKey first_key;
  if (!CreateValidIDBKeyFromValue(isolate, first, exception_state,
                                  &first_key)) {
    return 0;
  }
  IDBKey second_key;
  if (!CreateValidIDBKeyFromValue(isolate, second, exception_state,
                                  &second_key)) {
    return 0;
  }
  return static_cast<int16_t>(first_key.CompareTo(second_key));
}

}  // namespace mitto
