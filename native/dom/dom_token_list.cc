#include "mitto/dom/dom_token_list.h"

#include <algorithm>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_dom_token_list.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_parser_idioms.h"

namespace mitto {

namespace {

bool CheckEmptyToken(const std::u16string& token,
                     bindings::ExceptionState& exception_state) {
  if (!token.empty()) {
    return true;
  }
  exception_state.ThrowDOMException("SyntaxError",
                                    "The token provided must not be empty.");
  return false;
}

bool CheckTokenWithWhitespace(const std::u16string& token,
                              bindings::ExceptionState& exception_state) {
  if (std::none_of(token.begin(), token.end(), IsHTMLSpace)) {
    return true;
  }
  exception_state.ThrowDOMException(
      "InvalidCharacterError",
      base::StrCat({"The token provided ('", base::UTF16ToUTF8(token),
                    "') contains HTML space characters, which are not valid "
                    "in tokens."}));
  return false;
}

bool CheckTokenSyntax(const std::u16string& token,
                      bindings::ExceptionState& exception_state) {
  return CheckEmptyToken(token, exception_state) &&
         CheckTokenWithWhitespace(token, exception_state);
}

bool CheckTokensSyntax(const std::vector<std::u16string>& tokens,
                       bindings::ExceptionState& exception_state) {
  return std::all_of(tokens.begin(), tokens.end(),
                     [&exception_state](const std::u16string& token) {
                       return CheckTokenSyntax(token, exception_state);
                     });
}

bool Has(const std::vector<std::u16string>& tokens,
         const std::u16string& token) {
  return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

}  // namespace

// static
DOMTokenList* DOMTokenList::Create(Element& element,
                                   std::u16string attribute_name) {
  return bindings::MakeGarbageCollected<DOMTokenList>(
      &element, std::move(attribute_name));
}

// static
bindings::ScriptWrappable* DOMTokenList::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return bindings::MakeForKnownGap<DOMTokenList>(
      info, bindings::V8DOMTokenList::kWrapperTypeInfo, nullptr,
      std::u16string());
}

DOMTokenList::DOMTokenList(Element* element, std::u16string attribute_name)
    : element_(element), attribute_name_(std::move(attribute_name)) {}

DOMTokenList::~DOMTokenList() = default;

const bindings::WrapperTypeInfo* DOMTokenList::GetWrapperTypeInfo() const {
  return &bindings::V8DOMTokenList::kWrapperTypeInfo;
}

void DOMTokenList::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(element_);
}

std::vector<std::u16string> DOMTokenList::Tokens() const {
  std::vector<std::u16string> tokens;
  const std::u16string* value =
      element_ ? element_->FindAttribute(attribute_name_) : nullptr;
  if (!value) {
    return tokens;
  }
  size_t start = 0;
  while (start < value->size()) {
    while (start < value->size() && IsHTMLSpace((*value)[start])) {
      ++start;
    }
    size_t end = start;
    while (end < value->size() && !IsHTMLSpace((*value)[end])) {
      ++end;
    }
    if (end > start) {
      std::u16string token = value->substr(start, end - start);
      if (!Has(tokens, token)) {
        tokens.push_back(std::move(token));
      }
    }
    start = end;
  }
  return tokens;
}

void DOMTokenList::Update(const std::vector<std::u16string>& tokens) {
  // Blink's SpaceSplitString::SerializeToString().
  SetValue(base::JoinString(tokens, u" "));
}

uint32_t DOMTokenList::Length() const {
  return static_cast<uint32_t>(Tokens().size());
}

std::optional<std::u16string> DOMTokenList::Item(uint32_t index) const {
  const std::vector<std::u16string> tokens = Tokens();
  if (index >= tokens.size()) {
    return std::nullopt;
  }
  return tokens[index];
}

bool DOMTokenList::Contains(const std::u16string& token) const {
  return Has(Tokens(), token);
}

void DOMTokenList::Add(const std::vector<std::u16string>& tokens,
                       bindings::ExceptionState& exception_state) {
  if (!CheckTokensSyntax(tokens, exception_state)) {
    return;
  }
  std::vector<std::u16string> set = Tokens();
  for (const std::u16string& token : tokens) {
    if (!Has(set, token)) {
      set.push_back(token);
    }
  }
  Update(set);
}

void DOMTokenList::Remove(const std::vector<std::u16string>& tokens,
                          bindings::ExceptionState& exception_state) {
  if (!CheckTokensSyntax(tokens, exception_state)) {
    return;
  }
  // As Blink, which leaves a missing attribute missing.
  if (!element_ || !element_->FindAttribute(attribute_name_)) {
    return;
  }
  std::vector<std::u16string> set = Tokens();
  std::erase_if(set, [&tokens](const std::u16string& token) {
    return Has(tokens, token);
  });
  Update(set);
}

bool DOMTokenList::Toggle(const std::u16string& token,
                          std::optional<bool> force,
                          bindings::ExceptionState& exception_state) {
  if (!CheckTokenSyntax(token, exception_state)) {
    return false;
  }
  std::vector<std::u16string> set = Tokens();
  const bool present = Has(set, token);
  if (!force) {
    if (present) {
      std::erase(set, token);
    } else {
      set.push_back(token);
    }
    Update(set);
    return !present;
  }
  // Blink's SetTokenPresence(): the attribute changes only when the token's
  // presence does.
  if (*force != present) {
    if (*force) {
      set.push_back(token);
    } else {
      std::erase(set, token);
    }
    Update(set);
  }
  return *force;
}

bool DOMTokenList::Replace(const std::u16string& token,
                           const std::u16string& new_token,
                           bindings::ExceptionState& exception_state) {
  if (!CheckEmptyToken(token, exception_state) ||
      !CheckEmptyToken(new_token, exception_state) ||
      !CheckTokenWithWhitespace(token, exception_state) ||
      !CheckTokenWithWhitespace(new_token, exception_state)) {
    return false;
  }
  // The Infra Standard's replace in an ordered set, as Blink runs it.
  std::vector<std::u16string> set = Tokens();
  bool found_old_token = false;
  bool found_new_token = false;
  bool did_update = false;
  for (size_t i = 0; i < set.size(); ++i) {
    if (found_old_token) {
      if (set[i] == new_token) {
        set.erase(set.begin() + i);
        break;
      }
    } else if (found_new_token) {
      if (set[i] == token) {
        set.erase(set.begin() + i);
        did_update = true;
        break;
      }
    } else if (set[i] == token) {
      found_old_token = true;
      set[i] = new_token;
      did_update = true;
    } else if (set[i] == new_token) {
      found_new_token = true;
    }
  }
  if (!did_update) {
    return false;
  }
  Update(set);
  return true;
}

bool DOMTokenList::Supports(const std::u16string& token,
                            bindings::ExceptionState& exception_state) {
  return ValidateTokenValue(base::ToLowerASCII(token), exception_state);
}

bool DOMTokenList::ValidateTokenValue(
    const std::u16string& token,
    bindings::ExceptionState& exception_state) const {
  exception_state.ThrowTypeError("DOMTokenList has no supported tokens.");
  return false;
}

std::u16string DOMTokenList::Value() const {
  const std::u16string* value =
      element_ ? element_->FindAttribute(attribute_name_) : nullptr;
  return value ? *value : std::u16string();
}

void DOMTokenList::SetValue(const std::u16string& value) {
  if (element_) {
    element_->SetAttributeWithoutValidation(attribute_name_, value);
  }
}

}  // namespace mitto
