#include "mitto/dom/crypto.h"

#include <stddef.h>
#include <stdint.h>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "mitto/bindings/v8_crypto.h"
#include "mitto/dom/quota_exceeded_error.h"
#include "v8/include/v8-typed-array.h"

namespace mitto {

namespace {

// The most bytes that one call fills, as the specification has it.
constexpr size_t kMaxBytes = 65536;

// What Blink's DOMArrayBufferView::TypeName() calls a view that is not of an
// integer type, or nullptr for one that is.
const char* NonIntegerTypeName(v8::Local<v8::ArrayBufferView> view) {
  if (view->IsFloat16Array()) {
    return "Float16";
  }
  if (view->IsFloat32Array()) {
    return "Float32";
  }
  if (view->IsFloat64Array()) {
    return "Float64";
  }
  if (view->IsDataView()) {
    return "DataView";
  }
  return nullptr;
}

}  // namespace

// static
Crypto* Crypto::Create() {
  return bindings::MakeGarbageCollected<Crypto>();
}

Crypto::Crypto() = default;
Crypto::~Crypto() = default;

const bindings::WrapperTypeInfo* Crypto::GetWrapperTypeInfo() const {
  return &bindings::V8Crypto::kWrapperTypeInfo;
}

v8::Local<v8::ArrayBufferView> Crypto::GetRandomValues(
    v8::Local<v8::ArrayBufferView> array,
    bindings::ExceptionState& exception_state) {
  if (const char* const type_name = NonIntegerTypeName(array)) {
    exception_state.ThrowDOMException(
        "TypeMismatchError",
        base::StrCat({"The provided ArrayBufferView is of type '", type_name,
                      "', which is not an integer array type."}));
    return {};
  }
  const size_t length = array->ByteLength();
  if (length > kMaxBytes) {
    QuotaExceededError::Throw(
        exception_state,
        base::StrCat({"The ArrayBufferView's byte length (",
                      base::NumberToString(length),
                      ") exceeds the number of bytes of entropy available via "
                      "this API (65536)."}));
    return {};
  }
  if (length == 0) {
    return array;
  }
  // The bytes the view sees, which a view that V8 keeps on its heap gets a
  // buffer for first.
  const v8::Local<v8::ArrayBuffer> buffer = array->Buffer();
  uint8_t* const data = UNSAFE_BUFFERS(static_cast<uint8_t*>(buffer->Data()) +
                                       array->ByteOffset());
  base::RandBytes(UNSAFE_BUFFERS(base::span(data, length)));
  return array;
}

std::u16string Crypto::RandomUUID() {
  return base::ASCIIToUTF16(base::Uuid::GenerateRandomV4().AsLowercaseString());
}

}  // namespace mitto
