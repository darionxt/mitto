// Web IDL's DOMException, which the bindings throw for the errors that the DOM
// names, such as "InvalidStateError".
//
// Upstream: third_party/blink/renderer/core/dom/dom_exception.h
// Upstream: third_party/blink/renderer/core/dom/dom_exception.cc

#ifndef MITTO_DOM_DOM_EXCEPTION_H_
#define MITTO_DOM_DOM_EXCEPTION_H_

#include <stdint.h>

#include <string>
#include <string_view>

#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class DOMException final : public bindings::ScriptWrappable {
 public:
  static DOMException* Create(const std::u16string& message,
                              const std::u16string& name);

  DOMException(const std::u16string& message, const std::u16string& name);

  // Makes the wrapper of a new DOMException, for the bindings to throw.
  static v8::Local<v8::Value> CreateForBindings(v8::Isolate* isolate,
                                                std::string_view name,
                                                std::string_view message);

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint16_t Code() const;
  std::u16string Name() const { return name_; }
  std::u16string Message() const { return message_; }

 private:
  const std::u16string message_;
  const std::u16string name_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_EXCEPTION_H_
