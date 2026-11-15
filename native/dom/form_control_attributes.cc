#include "mitto/dom/form_control_attributes.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_parser_idioms.h"
#include "url/gurl.h"

namespace mitto {

namespace {

// The autofill field names of each category, and how many tokens a value of
// that category may have, as Blink's AutofillCategory table holds them.
enum class AutofillCategory {
  kNone,
  kOff,
  kAutomatic,
  kNormal,
  kContact,
  kCredential
};

constexpr std::array<std::u16string_view, 44> kNormalFields = {
    u"name",
    u"honorific-prefix",
    u"given-name",
    u"additional-name",
    u"family-name",
    u"honorific-suffix",
    u"nickname",
    u"organization-title",
    u"username",
    u"new-password",
    u"current-password",
    u"one-time-code",
    u"organization",
    u"street-address",
    u"address-line1",
    u"address-line2",
    u"address-line3",
    u"address-level4",
    u"address-level3",
    u"address-level2",
    u"address-level1",
    u"country",
    u"country-name",
    u"postal-code",
    u"cc-name",
    u"cc-given-name",
    u"cc-additional-name",
    u"cc-family-name",
    u"cc-number",
    u"cc-exp",
    u"cc-exp-month",
    u"cc-exp-year",
    u"cc-csc",
    u"cc-type",
    u"transaction-currency",
    u"transaction-amount",
    u"language",
    u"bday",
    u"bday-day",
    u"bday-month",
    u"bday-year",
    u"sex",
    u"url",
    u"photo",
};

constexpr std::array<std::u16string_view, 10> kContactFields = {
    u"tel",
    u"tel-country-code",
    u"tel-national",
    u"tel-area-code",
    u"tel-local",
    u"tel-local-prefix",
    u"tel-local-suffix",
    u"tel-extension",
    u"email",
    u"impp",
};

AutofillCategory CategoryOf(std::u16string_view token) {
  if (token == u"off") {
    return AutofillCategory::kOff;
  }
  if (token == u"on") {
    return AutofillCategory::kAutomatic;
  }
  if (token == u"webauthn") {
    return AutofillCategory::kCredential;
  }
  if (std::ranges::find(kContactFields, token) != kContactFields.end()) {
    return AutofillCategory::kContact;
  }
  if (std::ranges::find(kNormalFields, token) != kNormalFields.end()) {
    return AutofillCategory::kNormal;
  }
  return AutofillCategory::kNone;
}

size_t MaxTokens(AutofillCategory category) {
  switch (category) {
    case AutofillCategory::kOff:
    case AutofillCategory::kAutomatic:
      return 1;
    case AutofillCategory::kNormal:
      return 3;
    case AutofillCategory::kContact:
      return 4;
    case AutofillCategory::kCredential:
      return 5;
    case AutofillCategory::kNone:
      return 0;
  }
}

// SpaceSplitString of the lowercased value: its tokens, each once.
std::vector<std::u16string> AutofillTokens(std::u16string_view value) {
  std::vector<std::u16string> tokens;
  size_t start = 0;
  while (start < value.size()) {
    while (start < value.size() && IsHTMLSpace(value[start])) {
      ++start;
    }
    size_t end = start;
    while (end < value.size() && !IsHTMLSpace(value[end])) {
      ++end;
    }
    if (end > start) {
      std::u16string token(value.substr(start, end - start));
      std::ranges::transform(token, token.begin(),
                             [](char16_t c) { return base::ToLowerASCII(c); });
      if (std::ranges::find(tokens, token) == tokens.end()) {
        tokens.push_back(std::move(token));
      }
    }
    start = end;
  }
  return tokens;
}

void ThrowNotPositive(int32_t value,
                      bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException(
      "IndexSizeError",
      base::StrCat({"The value provided (", base::NumberToString(value),
                    ") is not positive or 0."}));
}

}  // namespace

std::u16string_view ParseFormEncodingType(std::u16string_view type) {
  if (base::EqualsCaseInsensitiveASCII(type, u"multipart/form-data")) {
    return u"multipart/form-data";
  }
  if (base::EqualsCaseInsensitiveASCII(type, u"text/plain")) {
    return u"text/plain";
  }
  return u"application/x-www-form-urlencoded";
}

std::u16string_view ParseFormMethod(std::u16string_view method) {
  if (base::EqualsCaseInsensitiveASCII(method, u"post")) {
    return u"post";
  }
  if (base::EqualsCaseInsensitiveASCII(method, u"dialog")) {
    return u"dialog";
  }
  return u"get";
}

std::u16string FormEnctypeOf(const Element& element) {
  const std::u16string* const value = element.FindAttribute(u"formenctype");
  return value ? std::u16string(ParseFormEncodingType(*value)) : u"";
}

std::u16string FormMethodOf(const Element& element) {
  const std::u16string* const value = element.FindAttribute(u"formmethod");
  return value ? std::u16string(ParseFormMethod(*value)) : u"";
}

std::u16string FormActionOf(const Element& element, std::u16string_view name) {
  const std::u16string* const value = element.FindAttribute(name);
  const Document& document = element.GetDocument();
  if (!value || value->empty()) {
    return document.UrlForBinding();
  }
  return base::UTF8ToUTF16(
      document.CompleteUrl(StripHTMLSpaces(*value)).possibly_invalid_spec());
}

std::u16string ExposedAutofillValue(const Element& element,
                                    bool is_hidden_input) {
  const std::u16string* const value = element.FindAttribute(u"autocomplete");
  if (!value) {
    return u"";
  }
  const std::vector<std::u16string> tokens = AutofillTokens(*value);
  if (tokens.empty()) {
    return u"";
  }
  size_t index = tokens.size() - 1;
  AutofillCategory category = CategoryOf(tokens[index]);
  const size_t max_tokens = MaxTokens(category);
  if (category == AutofillCategory::kNone || tokens.size() > max_tokens) {
    return u"";
  }
  if (category == AutofillCategory::kOff ||
      category == AutofillCategory::kAutomatic) {
    if (is_hidden_input) {
      return u"";
    }
    return category == AutofillCategory::kOff ? u"off" : u"on";
  }
  std::u16string result = tokens[index];
  if (category == AutofillCategory::kCredential && index != 0) {
    --index;
    category = CategoryOf(tokens[index]);
    if ((category != AutofillCategory::kNormal &&
         category != AutofillCategory::kContact) ||
        index > MaxTokens(category) - 1) {
      return u"";
    }
    result = base::StrCat({tokens[index], u" ", result});
  }
  if (index == 0) {
    return result;
  }
  --index;
  if (category == AutofillCategory::kContact &&
      (tokens[index] == u"home" || tokens[index] == u"work" ||
       tokens[index] == u"mobile" || tokens[index] == u"fax" ||
       tokens[index] == u"pager")) {
    result = base::StrCat({tokens[index], u" ", result});
    if (index == 0) {
      return result;
    }
    --index;
  }
  if (tokens[index] == u"shipping" || tokens[index] == u"billing") {
    result = base::StrCat({tokens[index], u" ", result});
    if (index == 0) {
      return result;
    }
    --index;
  }
  if (index != 0 || !tokens[index].starts_with(u"section-")) {
    return u"";
  }
  return base::StrCat({tokens[index], u" ", result});
}

int32_t TextControlMaxLength(const Element& element) {
  const std::u16string* const value = element.FindAttribute(u"maxlength");
  const std::optional<int32_t> length =
      value ? ParseHTMLInteger(*value) : std::nullopt;
  return length && *length >= 0 ? *length : -1;
}

int32_t TextControlMinLength(const Element& element) {
  const std::u16string* const value = element.FindAttribute(u"minlength");
  const std::optional<int32_t> length =
      value ? ParseHTMLInteger(*value) : std::nullopt;
  return length && *length >= 0 ? *length : -1;
}

void SetTextControlMaxLength(Element& element,
                             int32_t value,
                             bindings::ExceptionState& exception_state) {
  const int32_t min = TextControlMinLength(element);
  if (value < 0) {
    ThrowNotPositive(value, exception_state);
    return;
  }
  if (min >= 0 && value < min) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The maxLength provided (", base::NumberToString(value),
                      ") is less than the minimum bound (",
                      base::NumberToString(min), ")."}));
    return;
  }
  element.SetReflectedIntegerAttribute(u"maxlength", value);
}

void SetTextControlMinLength(Element& element,
                             int32_t value,
                             bindings::ExceptionState& exception_state) {
  const int32_t max = TextControlMaxLength(element);
  if (value < 0) {
    ThrowNotPositive(value, exception_state);
    return;
  }
  if (max >= 0 && value > max) {
    exception_state.ThrowDOMException(
        "IndexSizeError",
        base::StrCat({"The minLength provided (", base::NumberToString(value),
                      ") is greater than the maximum bound (",
                      base::NumberToString(max), ")."}));
    return;
  }
  element.SetReflectedIntegerAttribute(u"minlength", value);
}

}  // namespace mitto
