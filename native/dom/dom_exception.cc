#include "mitto/dom/dom_exception.h"

#include <array>
#include <string_view>

#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/v8_dom_exception.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"

namespace mitto {

namespace {

struct LegacyCode {
  std::u16string_view name;
  uint16_t code;
};

// The names that have a legacy code, from Web IDL's table of error names.
constexpr auto kLegacyCodes = std::to_array<LegacyCode>({
    {u"IndexSizeError", 1},
    {u"HierarchyRequestError", 3},
    {u"WrongDocumentError", 4},
    {u"InvalidCharacterError", 5},
    {u"NoModificationAllowedError", 7},
    {u"NotFoundError", 8},
    {u"NotSupportedError", 9},
    {u"InUseAttributeError", 10},
    {u"InvalidStateError", 11},
    {u"SyntaxError", 12},
    {u"InvalidModificationError", 13},
    {u"NamespaceError", 14},
    {u"InvalidAccessError", 15},
    {u"TypeMismatchError", 17},
    {u"SecurityError", 18},
    {u"NetworkError", 19},
    {u"AbortError", 20},
    {u"URLMismatchError", 21},
    {u"QuotaExceededError", 22},
    {u"TimeoutError", 23},
    {u"InvalidNodeTypeError", 24},
    {u"DataCloneError", 25},
});

}  // namespace

// static
DOMException* DOMException::Create(const std::u16string& message,
                                   const std::u16string& name) {
  return bindings::MakeGarbageCollected<DOMException>(message, name);
}

DOMException::DOMException(const std::u16string& message,
                           const std::u16string& name)
    : message_(message), name_(name) {}

DOMException::~DOMException() = default;

// static
bindings::PerIsolateData::DOMExceptionHooks DOMException::BindingsHooks() {
  bindings::PerIsolateData::DOMExceptionHooks hooks;
  // Blink's V8ThrowDOMException::CreateOrEmpty(): one that the bindings throw
  // gets the stack where it is thrown, which `new DOMException()` does not.
  hooks.create = [](v8::Isolate* isolate, std::string_view name,
                    std::string_view message) -> v8::Local<v8::Value> {
    const v8::Local<v8::Object> wrapper =
        Create(base::UTF8ToUTF16(message), base::UTF8ToUTF16(name))
            ->ToV8(isolate);
    if (!isolate->IsExecutionTerminating()) {
      v8::Exception::CaptureStackTrace(isolate->GetCurrentContext(), wrapper);
    }
    return wrapper;
  };
  // Blink's DOMException::AddContextToMessages().
  hooks.add_context = [](v8::Isolate* isolate, v8::Local<v8::Value> exception,
                         v8::ExceptionContext context_type,
                         std::string_view class_name,
                         std::string_view property_name) {
    DOMException* const dom_exception =
        bindings::V8DOMException::ToWrappable(isolate, exception);
    if (!dom_exception) {
      return false;
    }
    dom_exception->message_ =
        base::UTF8ToUTF16(bindings::ExceptionMessages::AddContextToMessage(
            context_type, class_name, property_name,
            base::UTF16ToUTF8(dom_exception->message_)));
    return true;
  };
  return hooks;
}

const bindings::WrapperTypeInfo* DOMException::GetWrapperTypeInfo() const {
  return &bindings::V8DOMException::kWrapperTypeInfo;
}

uint16_t DOMException::Code() const {
  for (const LegacyCode& entry : kLegacyCodes) {
    if (entry.name == name_) {
      return entry.code;
    }
  }
  return 0;
}

}  // namespace mitto
