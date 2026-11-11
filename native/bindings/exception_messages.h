// The messages of the errors that the bindings throw, as Chrome words them.
//
// Upstream: third_party/blink/renderer/platform/bindings/exception_messages.h
// Upstream: third_party/blink/renderer/platform/bindings/exception_messages.cc

#ifndef MITTO_BINDINGS_EXCEPTION_MESSAGES_H_
#define MITTO_BINDINGS_EXCEPTION_MESSAGES_H_

#include <string>
#include <string_view>

#include "v8/include/v8-exception.h"

namespace mitto::bindings {

class ExceptionMessages {
 public:
  ExceptionMessages() = delete;

  // Puts `message` in the context of the call that throws it, such as
  // "Failed to execute 'x' on 'Y': message".
  static std::string AddContextToMessage(v8::ExceptionContext context,
                                         std::string_view class_name,
                                         std::string_view property_name,
                                         std::string_view message);

  static std::string FailedToConvertJSValue(std::string_view type);
  static std::string FailedToConstruct(std::string_view type,
                                       std::string_view detail);
  static std::string FailedToExecute(std::string_view method,
                                     std::string_view type,
                                     std::string_view detail);
  static std::string FailedToGet(std::string_view property,
                                 std::string_view type,
                                 std::string_view detail);
  static std::string FailedToSet(std::string_view property,
                                 std::string_view type,
                                 std::string_view detail);

  static std::string ConstructorNotCallableAsFunction(std::string_view type);
  static std::string ArgumentNotOfType(int argument_index,
                                       std::string_view expected_type);
  static std::string NotEnoughArguments(unsigned expected, unsigned provided);
  static std::string ValueNotOfType(std::string_view expected_type);

  // What a member whose behaviour the Runtime does not have yet throws: a Known
  // Gap, which Chrome has no message for.
  static std::string NotImplemented();
};

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_EXCEPTION_MESSAGES_H_
