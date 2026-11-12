#include "mitto/bindings/exception_messages.h"

#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"

namespace mitto::bindings {

namespace {

// The property in the context of an indexed or a named property, if V8 names
// one.
std::string OptionalIndexProperty(std::string_view property) {
  if (property.empty()) {
    return std::string();
  }
  return base::StrCat({" [", property, "]"});
}

std::string OptionalNameProperty(std::string_view property) {
  if (property.empty()) {
    return std::string();
  }
  return base::StrCat({" '", property, "'"});
}

}  // namespace

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
    case v8::ExceptionContext::kNamedEnumerator:
      return FailedToEnumerate(class_name, message);
    case v8::ExceptionContext::kIndexedGetter:
    case v8::ExceptionContext::kIndexedDescriptor:
    case v8::ExceptionContext::kIndexedQuery:
      return FailedToGetIndexed(property_name, class_name, message);
    case v8::ExceptionContext::kIndexedSetter:
    case v8::ExceptionContext::kIndexedDefiner:
      return FailedToSetIndexed(property_name, class_name, message);
    case v8::ExceptionContext::kIndexedDeleter:
      return FailedToDeleteIndexed(property_name, class_name, message);
    case v8::ExceptionContext::kNamedGetter:
    case v8::ExceptionContext::kNamedDescriptor:
    case v8::ExceptionContext::kNamedQuery:
      return FailedToGetNamed(property_name, class_name, message);
    case v8::ExceptionContext::kNamedSetter:
    case v8::ExceptionContext::kNamedDefiner:
      return FailedToSetNamed(property_name, class_name, message);
    case v8::ExceptionContext::kNamedDeleter:
      return FailedToDeleteNamed(property_name, class_name, message);
    case v8::ExceptionContext::kUnknown:
      return std::string(message);
  }
  NOTREACHED();
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
  return base::StrCat({"Failed to set the '", property, "' property on '", type,
                       "': ", detail});
}

// static
std::string ExceptionMessages::FailedToEnumerate(std::string_view type,
                                                 std::string_view detail) {
  if (detail.empty()) {
    return base::StrCat({"Failed to enumerate the properties of '", type, "'"});
  }
  return base::StrCat(
      {"Failed to enumerate the properties of '", type, "': ", detail});
}

// static
std::string ExceptionMessages::FailedToGetIndexed(std::string_view property,
                                                  std::string_view type,
                                                  std::string_view detail) {
  return base::StrCat({"Failed to read an indexed property",
                       OptionalIndexProperty(property), " from '", type,
                       "': ", detail});
}

// static
std::string ExceptionMessages::FailedToSetIndexed(std::string_view property,
                                                  std::string_view type,
                                                  std::string_view detail) {
  return base::StrCat({"Failed to set an indexed property",
                       OptionalIndexProperty(property), " on '", type,
                       "': ", detail});
}

// static
std::string ExceptionMessages::FailedToDeleteIndexed(std::string_view property,
                                                     std::string_view type,
                                                     std::string_view detail) {
  return base::StrCat({"Failed to delete an indexed property",
                       OptionalIndexProperty(property), " from '", type,
                       "': ", detail});
}

// static
std::string ExceptionMessages::FailedToGetNamed(std::string_view property,
                                                std::string_view type,
                                                std::string_view detail) {
  return base::StrCat({"Failed to read a named property",
                       OptionalNameProperty(property), " from '", type,
                       "': ", detail});
}

// static
std::string ExceptionMessages::FailedToSetNamed(std::string_view property,
                                                std::string_view type,
                                                std::string_view detail) {
  return base::StrCat({"Failed to set a named property",
                       OptionalNameProperty(property), " on '", type,
                       "': ", detail});
}

// static
std::string ExceptionMessages::FailedToDeleteNamed(std::string_view property,
                                                   std::string_view type,
                                                   std::string_view detail) {
  return base::StrCat({"Failed to delete a named property",
                       OptionalNameProperty(property), " from '", type,
                       "': ", detail});
}

// static
std::string ExceptionMessages::ConstructorCalledAsFunction() {
  return "Please use the 'new' operator, this DOM object constructor cannot be "
         "called as a function.";
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
