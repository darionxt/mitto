#include "mitto/bindings/exception_messages.h"

#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"

namespace mitto::bindings {

// static
std::string ExceptionMessages::AddContextToMessage(
    v8::ExceptionContext context,
    std::string_view class_name,
    std::string_view property_name,
    std::string_view message) {
  switch (context) {
    case v8::ExceptionContext::kConstructor:
      return FailedToConstruct(class_name, message);
    case v8::ExceptionContext::kOperation:
      return FailedToExecute(property_name, class_name, message);
    case v8::ExceptionContext::kAttributeGet:
      return FailedToGet(property_name, class_name, message);
    case v8::ExceptionContext::kAttributeSet:
      return FailedToSet(property_name, class_name, message);
    case v8::ExceptionContext::kUnknown:
      return std::string(message);
    default:
      // The interceptors' contexts, which no interface the Runtime implements
      // has yet.
      NOTREACHED();
  }
}

// static
std::string ExceptionMessages::FailedToConvertJSValue(std::string_view type) {
  return base::StrCat({"Failed to convert value to '", type, "'."});
}

// static
std::string ExceptionMessages::FailedToConstruct(std::string_view type,
                                                 std::string_view detail) {
  if (type.empty()) {
    return std::string(detail);
  }
  if (detail.empty()) {
    return base::StrCat({"Failed to construct '", type, "'"});
  }
  return base::StrCat({"Failed to construct '", type, "': ", detail});
}

// static
std::string ExceptionMessages::FailedToExecute(std::string_view method,
                                               std::string_view type,
                                               std::string_view detail) {
  if (detail.empty()) {
    return base::StrCat({"Failed to execute '", method, "' on '", type, "'"});
  }
  return base::StrCat(
      {"Failed to execute '", method, "' on '", type, "': ", detail});
}

// static
std::string ExceptionMessages::FailedToGet(std::string_view property,
                                           std::string_view type,
                                           std::string_view detail) {
  return base::StrCat({"Failed to read the '", property, "' property from '",
                       type, "': ", detail});
}

// static
std::string ExceptionMessages::FailedToSet(std::string_view property,
                                           std::string_view type,
                                           std::string_view detail) {
  return base::StrCat({"Failed to set the '", property, "' property on '",
                       type, "': ", detail});
}

// static
std::string ExceptionMessages::ConstructorNotCallableAsFunction(
    std::string_view type) {
  return FailedToConstruct(type,
                           "Please use the 'new' operator, this DOM object "
                           "constructor cannot be called as a function.");
}

// static
std::string ExceptionMessages::ArgumentNotOfType(
    int argument_index,
    std::string_view expected_type) {
  return base::StrCat({"parameter ", base::NumberToString(argument_index + 1),
                       " is not of type '", expected_type, "'."});
}

// static
std::string ExceptionMessages::NotEnoughArguments(unsigned expected,
                                                  unsigned provided) {
  return base::StrCat({base::NumberToString(expected), " argument",
                       expected > 1 ? "s" : "", " required, but only ",
                       base::NumberToString(provided), " present."});
}

// static
std::string ExceptionMessages::ValueNotOfType(std::string_view expected_type) {
  return base::StrCat(
      {"The provided value is not of type '", expected_type, "'."});
}

// static
std::string ExceptionMessages::NotImplemented() {
  return "Not implemented.";
}

}  // namespace mitto::bindings
