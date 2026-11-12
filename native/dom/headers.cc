#include "mitto/dom/headers.h"

#include <variant>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "mitto/bindings/v8_headers.h"
#include "mitto/header_rules.h"

namespace mitto {
namespace {

constexpr std::string_view kInvalidName = "Invalid name";
constexpr std::string_view kInvalidValue = "Invalid value";
constexpr std::string_view kImmutable = "Headers are immutable";

}  // namespace

// static
Headers* Headers::Create(
    const std::optional<
        bindings::ByteStringSequenceSequenceOrByteStringByteStringRecord>& init,
    bindings::ExceptionState& exception_state) {
  Headers* const headers = Create(Guard::kNone);
  if (init) {
    headers->FillWith(*init, exception_state);
  }
  return headers;
}

// static
Headers* Headers::Create(Guard guard) {
  return bindings::MakeGarbageCollected<Headers>(guard);
}

Headers::Headers(Guard guard) : guard_(guard) {}

Headers::~Headers() = default;

const bindings::WrapperTypeInfo* Headers::GetWrapperTypeInfo() const {
  return &bindings::V8Headers::kWrapperTypeInfo;
}

void Headers::FillWith(
    const bindings::ByteStringSequenceSequenceOrByteStringByteStringRecord&
        init,
    bindings::ExceptionState& exception_state) {
  if (const auto* pairs =
          std::get_if<std::vector<std::vector<std::string>>>(&init)) {
    for (const std::vector<std::string>& pair : *pairs) {
      if (pair.size() != 2) {
        exception_state.ThrowTypeError(kInvalidValue);
        return;
      }
      Append(pair[0], pair[1], exception_state);
      if (exception_state.HadException()) {
        return;
      }
    }
    return;
  }
  for (const auto& [name, value] :
       std::get<std::vector<std::pair<std::string, std::string>>>(init)) {
    Append(name, value, exception_state);
    if (exception_state.HadException()) {
      return;
    }
  }
}

void Headers::Append(const std::string& name,
                     const std::string& value,
                     bindings::ExceptionState& exception_state) {
  const std::string_view normalized = NormalizeHeaderValue(value);
  if (!IsHttpToken(name)) {
    exception_state.ThrowTypeError(kInvalidName);
    return;
  }
  if (!IsValidHeaderValue(normalized)) {
    exception_state.ThrowTypeError(kInvalidValue);
    return;
  }
  if (guard_ == Guard::kImmutable) {
    exception_state.ThrowTypeError(kImmutable);
    return;
  }
  if (guard_ == Guard::kRequest && IsForbiddenRequestHeader(name, value)) {
    return;
  }
  if (guard_ == Guard::kResponse && IsForbiddenResponseHeaderName(name)) {
    return;
  }
  AppendLine(name, std::string(normalized));
}

void Headers::Remove(const std::string& name,
                     bindings::ExceptionState& exception_state) {
  if (!IsHttpToken(name)) {
    exception_state.ThrowTypeError(kInvalidName);
    return;
  }
  if (guard_ == Guard::kImmutable) {
    exception_state.ThrowTypeError(kImmutable);
    return;
  }
  if (guard_ == Guard::kRequest && IsForbiddenRequestHeader(name, "")) {
    return;
  }
  if (guard_ == Guard::kResponse && IsForbiddenResponseHeaderName(name)) {
    return;
  }
  std::erase_if(list_, [&name](const auto& line) {
    return base::EqualsCaseInsensitiveASCII(line.first, name);
  });
}

std::optional<std::string> Headers::Get(
    const std::string& name,
    bindings::ExceptionState& exception_state) {
  if (!IsHttpToken(name)) {
    exception_state.ThrowTypeError(kInvalidName);
    return std::nullopt;
  }
  return GetValue(name);
}

std::vector<std::string> Headers::GetSetCookie() const {
  std::vector<std::string> values;
  for (const auto& [name, value] : list_) {
    if (base::EqualsCaseInsensitiveASCII(name, "set-cookie")) {
      values.push_back(value);
    }
  }
  return values;
}

bool Headers::Has(const std::string& name,
                  bindings::ExceptionState& exception_state) {
  if (!IsHttpToken(name)) {
    exception_state.ThrowTypeError(kInvalidName);
    return false;
  }
  return GetValue(name).has_value();
}

void Headers::Set(const std::string& name,
                  const std::string& value,
                  bindings::ExceptionState& exception_state) {
  const std::string_view normalized = NormalizeHeaderValue(value);
  if (!IsHttpToken(name)) {
    exception_state.ThrowTypeError(kInvalidName);
    return;
  }
  if (!IsValidHeaderValue(normalized)) {
    exception_state.ThrowTypeError(kInvalidValue);
    return;
  }
  if (guard_ == Guard::kImmutable) {
    exception_state.ThrowTypeError(kImmutable);
    return;
  }
  if (guard_ == Guard::kRequest && IsForbiddenRequestHeader(name, value)) {
    return;
  }
  if (guard_ == Guard::kResponse && IsForbiddenResponseHeaderName(name)) {
    return;
  }
  // The Fetch Standard's set: the first line of the name takes the value, and
  // the others go.
  bool found = false;
  std::erase_if(list_, [&](std::pair<std::string, std::string>& line) {
    if (!base::EqualsCaseInsensitiveASCII(line.first, name)) {
      return false;
    }
    if (found) {
      return true;
    }
    found = true;
    line.second = std::string(normalized);
    return false;
  });
  if (!found) {
    list_.emplace_back(name, std::string(normalized));
  }
}

void Headers::AppendLine(std::string name, std::string value) {
  // A name that the list has keeps the spelling of its first line.
  for (const auto& line : list_) {
    if (base::EqualsCaseInsensitiveASCII(line.first, name)) {
      name = line.first;
      break;
    }
  }
  list_.emplace_back(std::move(name), std::move(value));
}

std::optional<std::string> Headers::GetValue(std::string_view name) const {
  std::optional<std::string> combined;
  for (const auto& [line_name, value] : list_) {
    if (!base::EqualsCaseInsensitiveASCII(line_name, name)) {
      continue;
    }
    if (combined) {
      base::StrAppend(&*combined, {", ", value});
    } else {
      combined = value;
    }
  }
  return combined;
}

std::string Headers::ToLines() const {
  std::string lines;
  for (const auto& [name, value] : list_) {
    base::StrAppend(&lines, {name, ": ", value, "\n"});
  }
  return lines;
}

}  // namespace mitto
