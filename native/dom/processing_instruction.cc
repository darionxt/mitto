#include "mitto/dom/processing_instruction.h"

#include <algorithm>

#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_processing_instruction.h"
#include "mitto/dom/markup_serializer.h"
#include "mitto/dom/qualified_name.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "third_party/icu/source/common/unicode/utf16.h"

namespace mitto {

namespace {

using Attributes = std::vector<std::pair<std::u16string, std::u16string>>;

// The code point at `index`, which moves past it, or nothing for a lone
// surrogate.
std::optional<UChar32> NextCodePoint(std::u16string_view text, size_t& index) {
  const char16_t lead = text[index++];
  if (!U16_IS_SURROGATE(lead)) {
    return lead;
  }
  if (U16_IS_SURROGATE_LEAD(lead) && index < text.size() &&
      U16_IS_TRAIL(text[index])) {
    return U16_GET_SUPPLEMENTARY(lead, text[index++]);
  }
  return std::nullopt;
}

bool HasCategory(UChar32 c, std::initializer_list<int8_t> categories) {
  return std::ranges::find(categories, u_charType(c)) != categories.end();
}

// Rules (c) and (d) of Blink's IsValidNameStart(): no compatibility character
// and no font or compatibility decomposition.
bool IsCompatibility(UChar32 c) {
  if (c >= 0xF900 && c < 0xFFFE) {
    return true;
  }
  const int32_t decomposition =
      u_getIntPropertyValue(c, UCHAR_DECOMPOSITION_TYPE);
  return decomposition == U_DT_FONT || decomposition == U_DT_COMPAT;
}

bool IsValidNameStart(UChar32 c) {
  if ((c >= 0x02BB && c <= 0x02C1) || c == 0x559 || c == 0x6E5 || c == 0x6E6 ||
      c == ':' || c == '_') {
    return true;
  }
  return HasCategory(c, {U_LOWERCASE_LETTER, U_UPPERCASE_LETTER, U_OTHER_LETTER,
                         U_TITLECASE_LETTER, U_LETTER_NUMBER}) &&
         !IsCompatibility(c);
}

bool IsValidNamePart(UChar32 c) {
  if (IsValidNameStart(c) || c == 0x00B7 || c == 0x0387 || c == '-' ||
      c == '.') {
    return true;
  }
  return HasCategory(
             c, {U_NON_SPACING_MARK, U_ENCLOSING_MARK, U_COMBINING_SPACING_MARK,
                 U_MODIFIER_LETTER, U_DECIMAL_DIGIT_NUMBER}) &&
         !IsCompatibility(c);
}

// XML's spaces, which separate attributes.
bool IsXMLSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// XML 1.0's Char.
bool IsXMLChar(UChar32 c) {
  return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) ||
         (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF);
}

// XML 1.0 fifth edition's NameStartChar and NameChar, which libxml2 reads an
// attribute's name by.
bool IsXMLNameStartChar(UChar32 c) {
  return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0xD6) ||
         (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) ||
         (c >= 0x370 && c <= 0x37D) || (c >= 0x37F && c <= 0x1FFF) ||
         (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) ||
         (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) ||
         (c >= 0xF900 && c <= 0xFDCF) || (c >= 0xFDF0 && c <= 0xFFFD) ||
         (c >= 0x10000 && c <= 0xEFFFF);
}

bool IsXMLNameChar(UChar32 c) {
  return IsXMLNameStartChar(c) || c == '-' || c == '.' ||
         (c >= '0' && c <= '9') || c == 0xB7 || (c >= 0x300 && c <= 0x36F) ||
         (c >= 0x203F && c <= 0x2040);
}

// A reference after '&' in an attribute's value, which moves past its ';':
// a character reference or one of XML's five entities, else nothing.
std::optional<std::u16string> ParseReference(std::u16string_view data,
                                             size_t& index) {
  const size_t end = data.find(';', index);
  if (end == std::u16string_view::npos) {
    return std::nullopt;
  }
  const std::u16string_view name = data.substr(index, end - index);
  index = end + 1;
  if (name.starts_with(u"#")) {
    const bool hex = name.starts_with(u"#x");
    const std::u16string_view digits = name.substr(hex ? 2 : 1);
    if (digits.empty()) {
      return std::nullopt;
    }
    UChar32 code_point = 0;
    for (const char16_t c : digits) {
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (hex && c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (hex && c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      }
      if (digit < 0) {
        return std::nullopt;
      }
      code_point = code_point * (hex ? 16 : 10) + digit;
      if (code_point > 0x10FFFF) {
        return std::nullopt;
      }
    }
    if (!IsXMLChar(code_point)) {
      return std::nullopt;
    }
    std::u16string text;
    if (U_IS_BMP(code_point)) {
      text += static_cast<char16_t>(code_point);
    } else {
      text += U16_LEAD(code_point);
      text += U16_TRAIL(code_point);
    }
    return text;
  }
  static constexpr std::pair<std::u16string_view, char16_t> kEntities[] = {
      {u"lt", '<'},
      {u"gt", '>'},
      {u"amp", '&'},
      {u"apos", '\''},
      {u"quot", '"'}};
  for (const auto& [entity, character] : kEntities) {
    if (name == entity) {
      return std::u16string(1, character);
    }
  }
  return std::nullopt;
}

// ParseAttributes(): the attributes of the start tag `<attrs DATA />`, as
// libxml2's SAX2 handler gets them, or nothing where libxml2 stops before the
// tag ends. Values have their references read and their spaces made ' ', and
// namespace declarations are none.
std::optional<Attributes> ParsePseudoAttributes(std::u16string_view data) {
  Attributes attributes;
  size_t index = 0;
  // The tag's own space comes before the first attribute.
  bool spaced = true;
  while (true) {
    while (index < data.size() && IsXMLSpace(data[index])) {
      ++index;
      spaced = true;
    }
    if (index == data.size()) {
      return attributes;
    }
    // The tag can end within the data, which closes it before libxml2 reads
    // what follows.
    if (data[index] == '>' || data.substr(index, 2) == u"/>") {
      return attributes;
    }
    if (!spaced) {
      return std::nullopt;
    }
    const size_t name_start = index;
    std::optional<UChar32> c = NextCodePoint(data, index);
    if (!c || !IsXMLNameStartChar(*c)) {
      return std::nullopt;
    }
    while (index < data.size()) {
      size_t next = index;
      c = NextCodePoint(data, next);
      if (!c || !IsXMLNameChar(*c)) {
        break;
      }
      index = next;
    }
    const std::u16string name(data.substr(name_start, index - name_start));
    while (index < data.size() && IsXMLSpace(data[index])) {
      ++index;
    }
    if (index == data.size() || data[index] != '=') {
      return std::nullopt;
    }
    ++index;
    while (index < data.size() && IsXMLSpace(data[index])) {
      ++index;
    }
    if (index == data.size() || (data[index] != '"' && data[index] != '\'')) {
      return std::nullopt;
    }
    const char16_t quote = data[index++];
    std::u16string value;
    while (true) {
      if (index == data.size()) {
        return std::nullopt;
      }
      const char16_t unit = data[index];
      if (unit == quote) {
        ++index;
        break;
      }
      if (unit == '<') {
        return std::nullopt;
      }
      if (unit == '&') {
        ++index;
        const std::optional<std::u16string> text = ParseReference(data, index);
        if (!text) {
          return std::nullopt;
        }
        value += *text;
        continue;
      }
      if (unit == '\r') {
        // A line break of CR LF is one space.
        ++index;
        if (index < data.size() && data[index] == '\n') {
          ++index;
        }
        value += u' ';
        continue;
      }
      size_t next = index;
      c = NextCodePoint(data, next);
      if (!c || !IsXMLChar(*c)) {
        return std::nullopt;
      }
      value += IsXMLSpace(unit) ? std::u16string_view(u" ")
                                : data.substr(index, next - index);
      index = next;
    }
    spaced = false;
    if (name == u"xmlns" || name.starts_with(u"xmlns:")) {
      continue;
    }
    if (std::ranges::any_of(
            attributes, [&name](const auto& a) { return a.first == name; })) {
      return std::nullopt;
    }
    attributes.emplace_back(name, std::move(value));
  }
}

}  // namespace

// static
ProcessingInstruction* ProcessingInstruction::Create(
    Document& document,
    const std::u16string& target,
    const std::u16string& data) {
  return bindings::MakeGarbageCollected<ProcessingInstruction>(document, target,
                                                               data);
}

ProcessingInstruction::ProcessingInstruction(Document& document,
                                             const std::u16string& target,
                                             const std::u16string& data)
    : CharacterData(document, data), target_(target) {}

ProcessingInstruction::~ProcessingInstruction() = default;

// static
bool ProcessingInstruction::IsValidName(std::u16string_view name) {
  if (name.empty()) {
    return false;
  }
  size_t index = 0;
  bool first = true;
  while (index < name.size()) {
    const std::optional<UChar32> c = NextCodePoint(name, index);
    if (!c || !(first ? IsValidNameStart(*c) : IsValidNamePart(*c))) {
      return false;
    }
    first = false;
  }
  return true;
}

const bindings::WrapperTypeInfo* ProcessingInstruction::GetWrapperTypeInfo()
    const {
  return &bindings::V8ProcessingInstruction::kWrapperTypeInfo;
}

uint16_t ProcessingInstruction::GetNodeType() const {
  return kProcessingInstructionNode;
}

std::u16string ProcessingInstruction::NodeName() const {
  return target_;
}

Node* ProcessingInstruction::CloneWithoutChildren(Document& document) const {
  return Create(document, target_, Data());
}

void ProcessingInstruction::ProcessAttributesIfNeeded() {
  if (parsed_data_ && *parsed_data_ == Data()) {
    return;
  }
  parsed_data_ = Data();
  attributes_ = ParsePseudoAttributes(Data()).value_or(Attributes());
}

std::optional<std::u16string> ProcessingInstruction::GetAttribute(
    const std::u16string& name) {
  ProcessAttributesIfNeeded();
  for (const auto& [key, value] : attributes_) {
    if (key == name) {
      return value;
    }
  }
  return std::nullopt;
}

bool ProcessingInstruction::HasAttribute(const std::u16string& name) {
  return GetAttribute(name).has_value();
}

void ProcessingInstruction::SetAttribute(
    const std::u16string& name,
    const std::u16string& value,
    bindings::ExceptionState& exception_state) {
  if (!IsValidAttributeLocalName(name)) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        base::StrCat({"Invalid attribute name: ", base::UTF16ToUTF8(name)}));
    return;
  }
  ProcessAttributesIfNeeded();
  const auto it = std::ranges::find_if(
      attributes_, [&name](const auto& a) { return a.first == name; });
  if (it != attributes_.end()) {
    it->second = value;
  } else {
    attributes_.emplace_back(name, value);
  }
  UpdateDataFromAttributes();
}

void ProcessingInstruction::RemoveAttribute(const std::u16string& name) {
  ProcessAttributesIfNeeded();
  const auto it = std::ranges::find_if(
      attributes_, [&name](const auto& a) { return a.first == name; });
  if (it == attributes_.end()) {
    return;
  }
  attributes_.erase(it);
  UpdateDataFromAttributes();
}

void ProcessingInstruction::ToggleAttribute(
    const std::u16string& name,
    std::optional<bool> force,
    bindings::ExceptionState& exception_state) {
  if (!IsValidAttributeLocalName(name)) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        base::StrCat({"Invalid attribute name: ", base::UTF16ToUTF8(name)}));
    return;
  }
  const bool present = HasAttribute(name);
  if (force.value_or(!present)) {
    if (!present) {
      SetAttribute(name, u"", exception_state);
    }
  } else {
    RemoveAttribute(name);
  }
}

bool ProcessingInstruction::HasAttributes() {
  ProcessAttributesIfNeeded();
  return !attributes_.empty();
}

std::vector<std::u16string> ProcessingInstruction::GetAttributeNames() {
  ProcessAttributesIfNeeded();
  std::vector<std::u16string> names;
  names.reserve(attributes_.size());
  for (const auto& [key, value] : attributes_) {
    names.push_back(key);
  }
  return names;
}

// MarkupFormatter::AppendAttributeValue() escapes each value as an HTML
// attribute's. Blink's SetDataFromAttributeChange() writes the data as
// observers hear of it, without moving live ranges in it and without reading
// it again, so the attributes stay as set even where XML would read the data
// otherwise.
void ProcessingInstruction::UpdateDataFromAttributes() {
  std::u16string data;
  for (size_t i = 0; i < attributes_.size(); ++i) {
    if (i) {
      data += u' ';
    }
    data += attributes_[i].first;
    data += u"=\"";
    data += EscapeAttributeValue(attributes_[i].second);
    data += u'"';
  }
  SetDataWithoutUpdate(data);
  parsed_data_ = data;
}

}  // namespace mitto
