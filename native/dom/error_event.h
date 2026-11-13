// The HTML Standard's ErrorEvent, which a window fires for an error that a
// script left uncaught, as Blink's ErrorEvent holds it.
//
// Upstream: third_party/blink/renderer/core/events/error_event.h
// Upstream: third_party/blink/renderer/core/events/error_event.cc

#ifndef MITTO_DOM_ERROR_EVENT_H_
#define MITTO_DOM_ERROR_EVENT_H_

#include <stdint.h>

#include <string>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class ErrorEvent final : public Event {
 public:
  static ErrorEvent* Create(const std::u16string& type,
                            const bindings::ErrorEventInit& init);
  // The error event of an error that a script left uncaught: it does not
  // bubble, and a listener can cancel it, which keeps it from the console.
  // `error` may be empty, for null.
  static ErrorEvent* CreateForScriptError(const std::u16string& message,
                                          const std::u16string& filename,
                                          uint32_t lineno,
                                          uint32_t colno,
                                          v8::Local<v8::Value> error);
  // What a window fires for an error of a script whose errors are muted, a
  // script of another origin fetched without CORS: "Script error.", with no
  // place and a null error, as Blink's CreateSanitizedError() makes it.
  static ErrorEvent* CreateSanitized();

  ErrorEvent(const std::u16string& type, const bindings::ErrorEventInit& init);
  ErrorEvent(const std::u16string& message,
             const std::u16string& filename,
             uint32_t lineno,
             uint32_t colno,
             v8::Local<v8::Value> error);
  ~ErrorEvent() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Message() const { return message_; }
  std::u16string Filename() const { return filename_; }
  uint32_t Lineno() const { return lineno_; }
  uint32_t Colno() const { return colno_; }
  v8::Local<v8::Value> Error() const;

  // Event:
  bool IsErrorEvent() const override;

 private:
  std::u16string message_;
  std::u16string filename_;
  uint32_t lineno_ = 0;
  uint32_t colno_ = 0;
  // Empty for null, as Blink keeps none for an init's error that is null.
  v8::TracedReference<v8::Value> error_;
};

}  // namespace mitto

#endif  // MITTO_DOM_ERROR_EVENT_H_
