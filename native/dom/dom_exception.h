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

#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"

namespace mitto {

class DOMException final : public bindings::ScriptWrappable {
 public:
  static DOMException* Create(const std::u16string& message,
                              const std::u16string& name);

  DOMException(const std::u16string& message, const std::u16string& name);

  // What the bindings need of a DOMException: see
  // bindings::PerIsolateData::DOMExceptionHooks.
  static bindings::PerIsolateData::DOMExceptionHooks BindingsHooks();

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;

  // The members of the interface.
  uint16_t Code() const;
  std::u16string Name() const { return name_; }
  std::u16string Message() const { return message_; }

 private:
  std::u16string message_;
  const std::u16string name_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOM_EXCEPTION_H_
