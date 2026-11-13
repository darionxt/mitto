#include "mitto/dom/pseudo_class_state.h"

#include <stdint.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/node.h"
#include "mitto/dom/qualified_name.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "third_party/icu/source/common/unicode/utf16.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-regexp.h"

namespace mitto::pseudo_class_state {

namespace {

constexpr double kMsPerDay = 86400000.0;

bool IsHTMLSpace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool HasAttribute(const Element& element, std::u16string_view name) {
  return element.FindAttribute(name) != nullptr;
}

bool IsHTML(const Node* node, std::u16string_view name) {
  return node && node->IsElementNode() &&
         static_cast<const Element*>(node)->HasHTMLTagName(name);
}

bool IsInNamespace(const Element& element,
                   std::u16string_view local_name,
                   std::u16string_view namespace_uri) {
  return element.LocalName() == local_name &&
         element.NamespaceURI() == namespace_uri;
}

bool IsOneOf(std::u16string_view value,
             std::initializer_list<std::u16string_view> values) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::u16string StripHTMLSpace(std::u16string_view text) {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && IsHTMLSpace(text[start])) {
    ++start;
  }
  while (end > start && IsHTMLSpace(text[end - 1])) {
    --end;
  }
  return std::u16string(text.substr(start, end - start));
}

std::u16string RemoveLineBreaks(std::u16string_view text) {
  std::u16string result;
  for (const char16_t c : text) {
    if (c != '\n' && c != '\r') {
      result += c;
    }
  }
  return result;
}

// Calls `visit` with each piece of `text` between commas.
template <typename Visit>
bool AnyCommaSeparated(std::u16string_view text, Visit visit) {
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(',', start);
    const size_t length =
        comma == std::u16string_view::npos ? comma : comma - start;
    if (visit(text.substr(start, length))) {
      return true;
    }
    if (comma == std::u16string_view::npos) {
      return false;
    }
    start = comma + 1;
  }
}

// D1: the type of an input, as Blink's InputType::NormalizeTypeName() gives
// it.
std::u16string InputType(const Element& input) {
  const std::u16string* type = input.FindAttribute(u"type");
  if (!type) {
    return u"text";
  }
  const std::u16string lower = base::ToLowerASCII(*type);
  if (IsOneOf(lower,
              {u"button", u"checkbox", u"color",  u"date",  u"datetime-local",
               u"email",  u"file",     u"hidden", u"image", u"month",
               u"number", u"password", u"radio",  u"range", u"reset",
               u"search", u"submit",   u"tel",    u"text",  u"time",
               u"url",    u"week"})) {
    return lower;
  }
  return u"text";
}

bool IsTextFieldType(std::u16string_view type) {
  return IsOneOf(type, {u"text", u"search", u"tel", u"url", u"email",
                        u"password", u"number"});
}

bool IsBaseTextType(std::u16string_view type) {
  return IsOneOf(type,
                 {u"text", u"search", u"tel", u"url", u"email", u"password"});
}

bool IsTemporalType(std::u16string_view type) {
  return IsOneOf(type,
                 {u"date", u"datetime-local", u"month", u"time", u"week"});
}

bool IsSteppableType(std::u16string_view type) {
  return IsTemporalType(type) || type == u"number" || type == u"range";
}

// Blink's ParseToDoubleForNumberType(): the HTML Standard's valid
// floating-point number.
std::optional<double> ParseNumber(std::u16string_view text) {
  if (text.empty() || text.back() == '.') {
    return std::nullopt;
  }
  const char16_t first = text.front();
  if (first != '-' && first != '.' && !base::IsAsciiDigit(first)) {
    return std::nullopt;
  }
  for (const char16_t c : text) {
    if (!base::IsAsciiDigit(c) && c != '-' && c != '+' && c != '.' &&
        c != 'e' && c != 'E') {
      return std::nullopt;
    }
  }
  double value;
  if (!base::StringToDouble(base::UTF16ToUTF8(text), &value) ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value == 0 ? 0 : value;
}

std::optional<int> ParseDigits(std::u16string_view text) {
  if (text.empty() || text.size() > 9) {
    return std::nullopt;
  }
  int value = 0;
  for (const char16_t c : text) {
    if (!base::IsAsciiDigit(c)) {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

bool IsLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
  constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
  return month == 2 && IsLeapYear(year) ? 29 : kDays[month - 1];
}

// Days from 1970-01-01.
int64_t DaysFromCivil(int64_t year, int month, int day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t year_of_era = year - era * 400;
  const int64_t day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

int64_t MondayOfFirstWeek(int64_t year) {
  const int64_t january_4 = DaysFromCivil(year, 1, 4);
  // 1970-01-01 was a Thursday, day 3 counting from Monday.
  return january_4 - ((january_4 + 3) % 7 + 7) % 7;
}

// Four digits or more, 1 to 275760, as Blink's DateComponents reads a year.
std::optional<int> ParseYear(std::u16string_view text) {
  if (text.size() < 4) {
    return std::nullopt;
  }
  const std::optional<int> year = ParseDigits(text);
  if (!year || *year < 1 || *year > 275760) {
    return std::nullopt;
  }
  return year;
}

// The HTML Standard's date string, as milliseconds at UTC midnight.
std::optional<double> ParseDate(std::u16string_view text) {
  const size_t dash = text.find('-');
  if (dash == std::u16string_view::npos || text.size() != dash + 6 ||
      text[dash + 3] != '-') {
    return std::nullopt;
  }
  const std::optional<int> year = ParseYear(text.substr(0, dash));
  const std::optional<int> month = ParseDigits(text.substr(dash + 1, 2));
  const std::optional<int> day = ParseDigits(text.substr(dash + 4, 2));
  if (!year || !month || !day || *month < 1 || *month > 12 || *day < 1 ||
      *day > DaysInMonth(*year, *month) ||
      (*year == 275760 && (*month > 9 || (*month == 9 && *day > 13)))) {
    return std::nullopt;
  }
  return DaysFromCivil(*year, *month, *day) * kMsPerDay;
}

// Months since 1970-01.
std::optional<double> ParseMonth(std::u16string_view text) {
  const size_t dash = text.find('-');
  if (dash == std::u16string_view::npos || text.size() != dash + 3) {
    return std::nullopt;
  }
  const std::optional<int> year = ParseYear(text.substr(0, dash));
  const std::optional<int> month = ParseDigits(text.substr(dash + 1, 2));
  if (!year || !month || *month < 1 || *month > 12 ||
      (*year == 275760 && *month > 9)) {
    return std::nullopt;
  }
  return (*year - 1970) * 12.0 + (*month - 1);
}

// Milliseconds since midnight: HH:MM, then :SS and a fraction if given.
std::optional<double> ParseTime(std::u16string_view text) {
  if (text.size() < 5 || text[2] != ':') {
    return std::nullopt;
  }
  const std::optional<int> hour = ParseDigits(text.substr(0, 2));
  const std::optional<int> minute = ParseDigits(text.substr(3, 2));
  if (!hour || !minute || *hour > 23 || *minute > 59) {
    return std::nullopt;
  }
  double ms = (*hour * 60.0 + *minute) * 60000.0;
  if (text.size() == 5) {
    return ms;
  }
  if (text.size() < 8 || text[5] != ':') {
    return std::nullopt;
  }
  const std::optional<int> second = ParseDigits(text.substr(6, 2));
  if (!second || *second > 59) {
    return std::nullopt;
  }
  ms += *second * 1000.0;
  if (text.size() == 8) {
    return ms;
  }
  if (text[8] != '.' || text.size() < 10 || text.size() > 12) {
    return std::nullopt;
  }
  const std::u16string_view fraction = text.substr(9);
  const std::optional<int> digits = ParseDigits(fraction);
  if (!digits) {
    return std::nullopt;
  }
  return ms + *digits * std::pow(10.0, 3 - static_cast<int>(fraction.size()));
}

std::optional<double> ParseDateTimeLocal(std::u16string_view text) {
  const size_t separator = text.find_first_of(u"T ");
  if (separator == std::u16string_view::npos) {
    return std::nullopt;
  }
  const std::optional<double> date = ParseDate(text.substr(0, separator));
  const std::optional<double> time = ParseTime(text.substr(separator + 1));
  if (!date || !time) {
    return std::nullopt;
  }
  return *date + *time;
}

// Milliseconds of the week's Monday at UTC midnight.
std::optional<double> ParseWeek(std::u16string_view text) {
  const size_t dash = text.find(u"-W");
  if (dash == std::u16string_view::npos || text.size() != dash + 4) {
    return std::nullopt;
  }
  const std::optional<int> year = ParseYear(text.substr(0, dash));
  const std::optional<int> week = ParseDigits(text.substr(dash + 2, 2));
  if (!year || !week || *week < 1) {
    return std::nullopt;
  }
  const int64_t first_monday = MondayOfFirstWeek(*year);
  const int64_t weeks = (MondayOfFirstWeek(*year + 1) - first_monday) / 7;
  if (*week > weeks) {
    return std::nullopt;
  }
  return (first_monday + (*week - 1) * 7) * kMsPerDay;
}

// A value of a steppable type, parsed for the type.
std::optional<double> ParseForType(std::u16string_view type,
                                   std::u16string_view text) {
  if (type == u"number" || type == u"range") {
    return ParseNumber(text);
  }
  if (type == u"date") {
    return ParseDate(text);
  }
  if (type == u"month") {
    return ParseMonth(text);
  }
  if (type == u"week") {
    return ParseWeek(text);
  }
  if (type == u"time") {
    return ParseTime(text);
  }
  if (type == u"datetime-local") {
    return ParseDateTimeLocal(text);
  }
  return std::nullopt;
}

std::optional<double> ParsedAttribute(const Element& input,
                                      std::u16string_view type,
                                      std::u16string_view name) {
  const std::u16string* text = input.FindAttribute(name);
  return text ? ParseForType(type, *text) : std::nullopt;
}

double DefaultMinimum(std::u16string_view type) {
  if (type == u"number") {
    return -std::numeric_limits<double>::max();
  }
  if (type == u"range" || type == u"time") {
    return 0;
  }
  if (type == u"month") {
    return (1 - 1970) * 12.0;
  }
  return DaysFromCivil(1, 1, 1) * kMsPerDay;
}

double DefaultMaximum(std::u16string_view type) {
  if (type == u"number") {
    return std::numeric_limits<double>::max();
  }
  if (type == u"range") {
    return 100;
  }
  if (type == u"time") {
    return kMsPerDay - 1;
  }
  if (type == u"month") {
    return (275760 - 1970) * 12.0 + 8;
  }
  return DaysFromCivil(275760, 9, 13) * kMsPerDay;
}

struct StepRange {
  double minimum;
  double maximum;
  bool has_minimum;
  bool has_maximum;

  // A time range whose maximum is below its minimum, which wraps around
  // midnight.
  bool reversed = false;
};

StepRange StepRangeOf(const Element& input, std::u16string_view type) {
  const std::optional<double> minimum = ParsedAttribute(input, type, u"min");
  const std::optional<double> maximum = ParsedAttribute(input, type, u"max");
  StepRange range{minimum.value_or(DefaultMinimum(type)),
                  maximum.value_or(DefaultMaximum(type)), minimum.has_value(),
                  maximum.has_value()};
  if (type == u"range" && range.maximum < range.minimum) {
    range.maximum = range.minimum;
  }
  range.reversed = type == u"time" &&
                   (range.has_minimum || range.has_maximum) &&
                   range.maximum < range.minimum;
  return range;
}

// Blink's RangeInputType::SanitizeValue(): the value, or the middle of the
// range, held in the range and rounded onto the step grid, which refuses
// "any", from the minimum, or the value attribute without one.
double SanitizedRangeValue(const Element& input, std::u16string_view raw) {
  const StepRange range = StepRangeOf(input, u"range");
  const double value = ParseNumber(raw).value_or(
      range.minimum + (range.maximum - range.minimum) / 2);
  const double in_range =
      std::min(std::max(value, range.minimum), range.maximum);
  const std::u16string* step_attribute = input.FindAttribute(u"step");
  std::optional<double> step =
      step_attribute ? ParseNumber(*step_attribute) : std::nullopt;
  if (!step || *step <= 0) {
    step = 1;
  }
  const std::u16string* min_attribute = input.FindAttribute(u"min");
  const std::u16string* value_attribute = input.FindAttribute(u"value");
  std::optional<double> base =
      min_attribute ? ParseNumber(*min_attribute) : std::nullopt;
  if (!base) {
    base = value_attribute ? ParseNumber(*value_attribute) : std::nullopt;
  }
  const double step_base = base.value_or(0);
  const double rounded =
      step_base + std::round((in_range - step_base) / *step) * *step;
  const double clamped = rounded > range.maximum   ? rounded - *step
                         : rounded < range.minimum ? rounded + *step
                                                   : rounded;
  // A step larger than the range leaves the value where it was held.
  return clamped < range.minimum || clamped > range.maximum ? in_range
                                                            : clamped;
}

// D2: the value of an untouched input, null where the value attribute is
// for a type whose value is the attribute itself.
std::optional<std::u16string> InputValue(const Element& input,
                                         std::u16string_view type) {
  const std::u16string* attribute = input.FindAttribute(u"value");
  if (IsOneOf(type, {u"hidden", u"submit", u"reset", u"button", u"image"})) {
    return attribute ? std::optional<std::u16string>(*attribute) : std::nullopt;
  }
  if (type == u"checkbox" || type == u"radio") {
    return attribute ? *attribute : u"on";
  }
  if (type == u"file") {
    return std::u16string();
  }
  const std::u16string raw = attribute ? *attribute : std::u16string();
  if (IsOneOf(type, {u"text", u"search", u"tel", u"password"})) {
    return RemoveLineBreaks(raw);
  }
  if (type == u"url") {
    return RemoveLineBreaks(StripHTMLSpace(raw));
  }
  if (type == u"email") {
    const std::u16string value = StripHTMLSpace(RemoveLineBreaks(raw));
    if (!HasAttribute(input, u"multiple")) {
      return value;
    }
    // Each address stripped, joined again with commas.
    std::u16string joined;
    AnyCommaSeparated(value, [&joined](std::u16string_view piece) {
      joined += StripHTMLSpace(piece);
      joined += u',';
      return false;
    });
    joined.pop_back();
    return joined;
  }
  if (type == u"range") {
    return base::UTF8ToUTF16(
        base::NumberToString(SanitizedRangeValue(input, raw)));
  }
  if (IsSteppableType(type)) {
    return ParseForType(type, raw) ? raw : std::u16string();
  }
  // color.
  return raw;
}

// A textarea's text: its Text children's data, with CR LF and CR as LF.
std::u16string TextAreaValue(const Element& textarea) {
  std::u16string text;
  for (const Node* child = textarea.FirstChild(); child;
       child = child->NextSibling()) {
    if (child->IsTextNode()) {
      text += static_cast<const CharacterData*>(child)->Data();
    }
  }
  std::u16string value;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\r') {
      value += text[i];
      continue;
    }
    value += u'\n';
    if (i + 1 < text.size() && text[i + 1] == '\n') {
      ++i;
    }
  }
  return value;
}

// D3: the form owner, by the form attribute or the nearest form ancestor.
// The parser's form element pointer, which can own a control outside its
// form in misnested markup, is not followed.
const Element* FormOwner(const Element& control) {
  if (const std::u16string* form_id = control.FindAttribute(u"form")) {
    if (!control.IsConnected()) {
      return nullptr;
    }
    const Node& root = control.TreeRoot();
    for (const Node* node = root.FirstChild(); node;
         node = node->NextInTree(&root)) {
      if (!node->IsElementNode()) {
        continue;
      }
      const auto& element = static_cast<const Element&>(*node);
      const std::u16string* id = element.FindAttribute(u"id");
      if (id && *id == *form_id) {
        return element.HasHTMLTagName(u"form") ? &element : nullptr;
      }
    }
    return nullptr;
  }
  for (const Element* ancestor = control.ParentElement(); ancestor;
       ancestor = ancestor->ParentElement()) {
    if (ancestor->HasHTMLTagName(u"form")) {
      return ancestor;
    }
  }
  return nullptr;
}

bool IsListedElement(const Element& element) {
  for (const std::u16string_view name :
       {u"button", u"fieldset", u"input", u"object", u"output", u"select",
        u"textarea"}) {
    if (element.HasHTMLTagName(name)) {
      return true;
    }
  }
  return false;
}

bool IsFormControl(const Element& element) {
  return element.HasHTMLTagName(u"button") ||
         element.HasHTMLTagName(u"input") ||
         element.HasHTMLTagName(u"select") ||
         element.HasHTMLTagName(u"textarea");
}

// The first legend element child of a fieldset.
const Element* LegendOf(const Element& fieldset) {
  for (const Node* child = fieldset.FirstChild(); child;
       child = child->NextSibling()) {
    if (IsHTML(child, u"legend")) {
      return static_cast<const Element*>(child);
    }
  }
  return nullptr;
}

// D4: whether a disabled fieldset ancestor disables `element`, but in that
// fieldset's first legend.
bool HasDisabledFieldSetAncestor(const Element& element) {
  const Element* last_legend = nullptr;
  for (const Element* ancestor = element.ParentElement(); ancestor;
       ancestor = ancestor->ParentElement()) {
    if (!ancestor->IsHTMLElement()) {
      continue;
    }
    if (ancestor->HasHTMLTagName(u"legend")) {
      last_legend = ancestor;
    }
    if (ancestor->HasHTMLTagName(u"fieldset") &&
        HasAttribute(*ancestor, u"disabled")) {
      // Blink's UpdateAncestorDisabledState(): only the fieldset's first
      // legend, if it has one, keeps what is inside it enabled.
      if (last_legend && last_legend == LegendOf(*ancestor)) {
        continue;
      }
      return true;
    }
  }
  return false;
}

bool IsActuallyDisabled(const Element& element) {
  return HasAttribute(element, u"disabled") ||
         HasDisabledFieldSetAncestor(element);
}

struct SelectParts {
  raw_ptr<const Element> select = nullptr;
  raw_ptr<const Element> optgroup = nullptr;
};

// D7: the select and optgroup of an option or optgroup.
SelectParts RelatedSelectParts(const Element& element) {
  SelectParts parts;
  const bool is_optgroup = element.HasHTMLTagName(u"optgroup");
  for (const Node* node = element.ParentNode(); node;
       node = node->ParentNode()) {
    if (IsHTML(node, u"option") || IsHTML(node, u"hr") ||
        IsHTML(node, u"datalist")) {
      return {nullptr, parts.optgroup};
    }
    if (IsHTML(node, u"optgroup")) {
      if (is_optgroup || parts.optgroup) {
        return {nullptr, parts.optgroup};
      }
      parts.optgroup = static_cast<const Element*>(node);
    }
    if (IsHTML(node, u"select")) {
      parts.select = static_cast<const Element*>(node);
      return parts;
    }
  }
  return parts;
}

bool IsOptionDisabledForSelection(const Element& option) {
  if (HasAttribute(option, u"disabled")) {
    return true;
  }
  const SelectParts parts = RelatedSelectParts(option);
  return parts.optgroup && HasAttribute(*parts.optgroup, u"disabled");
}

// A select's option list: its option descendants, not inside an option or
// an hr.
std::vector<const Element*> OptionsOf(const Element& select) {
  std::vector<const Element*> options;
  for (const Node* node = select.FirstChild(); node;) {
    if (IsHTML(node, u"option")) {
      options.push_back(static_cast<const Element*>(node));
      node = node->NextInTreeSkippingChildren(&select);
    } else if (IsHTML(node, u"hr")) {
      node = node->NextInTreeSkippingChildren(&select);
    } else {
      node = node->NextInTree(&select);
    }
  }
  return options;
}

// Blink's ParseHTMLNonNegativeInteger() of the size attribute, 0 for none.
int SelectSize(const Element& select) {
  const std::u16string* size = select.FindAttribute(u"size");
  if (!size) {
    return 0;
  }
  const std::u16string text = StripHTMLSpace(*size);
  size_t start = text.starts_with(u"+") ? 1 : 0;
  size_t end = start;
  while (end < text.size() && base::IsAsciiDigit(text[end])) {
    ++end;
  }
  int value = 0;
  if (end == start ||
      !base::StringToInt(base::UTF16ToUTF8(text.substr(start, end - start)),
                         &value)) {
    return 0;
  }
  return value;
}

// D7: whether an option is selected after parsing.
bool IsOptionSelected(const Element& option) {
  const SelectParts parts = RelatedSelectParts(option);
  if (!parts.select || HasAttribute(*parts.select, u"multiple")) {
    return HasAttribute(option, u"selected");
  }
  const std::vector<const Element*> options = OptionsOf(*parts.select);
  const Element* last_selected = nullptr;
  for (const Element* candidate : options) {
    if (HasAttribute(*candidate, u"selected")) {
      last_selected = candidate;
    }
  }
  if (last_selected) {
    return last_selected == &option;
  }
  if (SelectSize(*parts.select) > 1) {
    return false;
  }
  for (const Element* candidate : options) {
    if (!IsOptionDisabledForSelection(*candidate)) {
      return candidate == &option;
    }
  }
  return false;
}

// D6: a radio's group, the radios of its name in its form or tree, and the
// one of them that parsing left checked.
struct RadioGroup {
  std::vector<const Element*> members;
  raw_ptr<const Element> checked = nullptr;
};

std::optional<RadioGroup> RadioGroupOf(const Element& radio) {
  const std::u16string* name = radio.FindAttribute(u"name");
  if (!name || name->empty()) {
    return std::nullopt;
  }
  const Element* const owner = FormOwner(radio);
  RadioGroup group;
  const Node& root = radio.TreeRoot();
  for (const Node* node = &root; node; node = node->NextInTree(&root)) {
    if (!IsHTML(node, u"input")) {
      continue;
    }
    const auto& input = static_cast<const Element&>(*node);
    const std::u16string* input_name = input.FindAttribute(u"name");
    if (InputType(input) != u"radio" || !input_name || *input_name != *name ||
        FormOwner(input) != owner) {
      continue;
    }
    group.members.push_back(&input);
    if (HasAttribute(input, u"checked")) {
      group.checked = &input;
    }
  }
  return group;
}

bool IsRadioChecked(const Element& radio) {
  const std::optional<RadioGroup> group = RadioGroupOf(radio);
  return group ? group->checked == &radio : HasAttribute(radio, u"checked");
}

bool IsInputChecked(const Element& input, std::u16string_view type) {
  if (type == u"checkbox") {
    return HasAttribute(input, u"checked");
  }
  return type == u"radio" && IsRadioChecked(input);
}

// The button type state Submit of a button element.
bool IsSubmitButton(const Element& button) {
  if (const std::u16string* type = button.FindAttribute(u"type")) {
    if (base::EqualsCaseInsensitiveASCII(*type, u"submit")) {
      return true;
    }
    if (base::EqualsCaseInsensitiveASCII(*type, u"reset") ||
        base::EqualsCaseInsensitiveASCII(*type, u"button")) {
      return false;
    }
  }
  return !HasAttribute(button, u"command") &&
         !HasAttribute(button, u"commandfor");
}

bool CanBeSuccessfulSubmitButton(const Element& element) {
  if (element.HasHTMLTagName(u"input")) {
    return IsOneOf(InputType(element), {u"submit", u"image"});
  }
  if (!element.HasHTMLTagName(u"button") || !IsSubmitButton(element)) {
    return false;
  }
  // A select's slotted button: its first element child in menu-list mode.
  const Element* const parent = element.ParentElement();
  return !(parent && parent->HasHTMLTagName(u"select") &&
           parent->FirstElementChild() == &element &&
           !HasAttribute(*parent, u"multiple") && SelectSize(*parent) <= 1);
}

// Whether `element` is its form's default button: the first control, in
// tree order, that the form owns and that can submit, disabled or not.
bool IsDefaultButton(const Element& element) {
  const Element* const form = FormOwner(element);
  if (!form) {
    return false;
  }
  const Node& root = element.TreeRoot();
  for (const Node* node = &root; node; node = node->NextInTree(&root)) {
    if (!node->IsElementNode()) {
      continue;
    }
    const auto& candidate = static_cast<const Element&>(*node);
    if (IsListedElement(candidate) && FormOwner(candidate) == form &&
        CanBeSuccessfulSubmitButton(candidate)) {
      return &candidate == &element;
    }
  }
  return false;
}

bool IsInDataList(const Element& element) {
  for (const Element* ancestor = element.ParentElement(); ancestor;
       ancestor = ancestor->ParentElement()) {
    if (ancestor->HasHTMLTagName(u"datalist")) {
      return true;
    }
  }
  return false;
}

// D5: whether a form control is a candidate for constraint validation.
bool WillValidate(const Element& element) {
  if (IsInDataList(element) || IsActuallyDisabled(element)) {
    return false;
  }
  if ((element.HasHTMLTagName(u"input") ||
       element.HasHTMLTagName(u"textarea")) &&
      HasAttribute(element, u"readonly")) {
    return false;
  }
  if (element.HasHTMLTagName(u"input")) {
    return !IsOneOf(InputType(element),
                    {u"button", u"hidden", u"image", u"reset"});
  }
  if (element.HasHTMLTagName(u"button")) {
    return IsSubmitButton(element);
  }
  return element.HasHTMLTagName(u"select") ||
         element.HasHTMLTagName(u"textarea");
}

bool SupportsRequired(std::u16string_view type) {
  return IsTextFieldType(type) || IsTemporalType(type) ||
         IsOneOf(type, {u"checkbox", u"radio", u"file"});
}

// Blink's HTMLOptionElement::value(): its value attribute, or its text,
// stripped and with runs of whitespace as one space.
std::u16string OptionValue(const Element& option) {
  // An explicit value is the option's value as it is.
  if (const std::u16string* value = option.FindAttribute(u"value")) {
    return *value;
  }
  std::u16string text;
  for (const Node* node = option.FirstChild(); node;) {
    if (node->IsElementNode() &&
        static_cast<const Element*>(node)->LocalName() == u"script") {
      node = node->NextInTreeSkippingChildren(&option);
      continue;
    }
    if (node->IsTextNode()) {
      text += static_cast<const CharacterData*>(node)->Data();
    }
    node = node->NextInTree(&option);
  }
  std::u16string collapsed;
  bool in_space = false;
  for (const char16_t c : StripHTMLSpace(text)) {
    if (IsHTMLSpace(c)) {
      in_space = true;
      continue;
    }
    if (in_space) {
      collapsed += u' ';
      in_space = false;
    }
    collapsed += c;
  }
  return collapsed;
}

bool IsSelectValueMissing(const Element& select) {
  if (!HasAttribute(select, u"required")) {
    return false;
  }
  const Element* selected = nullptr;
  for (const Element* option : OptionsOf(select)) {
    if (IsOptionSelected(*option)) {
      selected = option;
      break;
    }
  }
  if (!selected) {
    return true;
  }
  // A placeholder label option: the first of the select's list items, an
  // option whose value is empty, in a select that is neither multiple nor a
  // list box.
  if (HasAttribute(select, u"multiple") || SelectSize(select) > 1) {
    return false;
  }
  for (const Node* node = select.FirstChild(); node;
       node = node->NextInTree(&select)) {
    if (IsHTML(node, u"option") || IsHTML(node, u"hr") ||
        IsHTML(node, u"optgroup")) {
      return node == selected &&
             OptionValue(static_cast<const Element&>(*node)).empty();
    }
  }
  return false;
}

// Whether an email address is valid, as the HTML Standard's pattern takes
// one. A non-ASCII domain, which Blink converts with UTS 46 before checking,
// is not valid here.
bool IsValidEmailAddress(std::u16string_view address) {
  const size_t at = address.find('@');
  if (at == std::u16string_view::npos || at == 0) {
    return false;
  }
  for (const char16_t c : address.substr(0, at)) {
    if (!base::IsAsciiAlphaNumeric(c) &&
        std::u16string_view(u"!#$%&'*+/=?^_`{|}~.-").find(c) ==
            std::u16string_view::npos) {
      return false;
    }
  }
  const std::u16string_view domain = address.substr(at + 1);
  size_t start = 0;
  while (true) {
    const size_t dot = domain.find('.', start);
    const std::u16string_view label = domain.substr(
        start, dot == std::u16string_view::npos ? dot : dot - start);
    if (label.empty() || label.size() > 63 ||
        !base::IsAsciiAlphaNumeric(label.front()) ||
        !base::IsAsciiAlphaNumeric(label.back())) {
      return false;
    }
    for (const char16_t c : label) {
      if (!base::IsAsciiAlphaNumeric(c) && c != '-') {
        return false;
      }
    }
    if (dot == std::u16string_view::npos) {
      return true;
    }
    start = dot + 1;
  }
}

bool IsTypeMismatch(const Element& input,
                    std::u16string_view type,
                    std::u16string_view value) {
  if (value.empty()) {
    return false;
  }
  if (type == u"email") {
    if (!HasAttribute(input, u"multiple")) {
      return !IsValidEmailAddress(value);
    }
    return AnyCommaSeparated(value, [](std::u16string_view piece) {
      return !IsValidEmailAddress(StripHTMLSpace(piece));
    });
  }
  if (type == u"url") {
    return !GURL(base::UTF16ToUTF8(value)).is_valid();
  }
  return false;
}

v8::MaybeLocal<v8::String> ToV8String(v8::Isolate* isolate,
                                      std::u16string_view text) {
  return v8::String::NewFromTwoByte(
      isolate, reinterpret_cast<const uint16_t*>(text.data()),
      v8::NewStringType::kNormal, static_cast<int>(text.size()));
}

// Whether `value` does not match all of `pattern`, compiled with the v
// flag, as Blink's BaseTextInputType::PatternMismatch() checks it. A pattern
// that does not compile matches anything.
bool MismatchesPattern(std::u16string_view pattern, std::u16string_view value) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate || !isolate->InContext()) {
    return false;
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::TryCatch try_catch(isolate);
  std::u16string source = u"^(?:";
  source += pattern;
  source += u")$";
  v8::Local<v8::String> source_string;
  v8::Local<v8::RegExp> regexp;
  v8::Local<v8::String> value_string;
  v8::Local<v8::Value> match;
  if (!ToV8String(isolate, source).ToLocal(&source_string) ||
      !v8::RegExp::New(context, source_string, v8::RegExp::kUnicodeSets)
           .ToLocal(&regexp) ||
      !ToV8String(isolate, value).ToLocal(&value_string) ||
      !regexp->Exec(context, value_string).ToLocal(&match)) {
    return false;
  }
  return match->IsNull();
}

bool IsPatternMismatch(const Element& input,
                       std::u16string_view type,
                       std::u16string_view value) {
  const std::u16string* pattern = input.FindAttribute(u"pattern");
  if (!pattern || value.empty() || !IsBaseTextType(type)) {
    return false;
  }
  if (type == u"email" && HasAttribute(input, u"multiple")) {
    return AnyCommaSeparated(value, [pattern](std::u16string_view piece) {
      return MismatchesPattern(*pattern, piece);
    });
  }
  return MismatchesPattern(*pattern, value);
}

// The step of a steppable type, scaled as the type's values are, as Blink's
// StepRange has it, or nullopt for "any".
std::optional<double> StepOf(const Element& input, std::u16string_view type) {
  double default_step = 1;
  double scale = 1;
  bool integral = false;
  if (type == u"date") {
    scale = kMsPerDay;
    integral = true;
  } else if (type == u"month") {
    integral = true;
  } else if (type == u"week") {
    scale = 7 * kMsPerDay;
    integral = true;
  } else if (type == u"time" || type == u"datetime-local") {
    default_step = 60;
    scale = 1000;
  }
  const std::u16string* attribute = input.FindAttribute(u"step");
  if (attribute && base::EqualsCaseInsensitiveASCII(*attribute, u"any")) {
    return std::nullopt;
  }
  std::optional<double> step =
      attribute ? ParseNumber(*attribute) : std::nullopt;
  if (!step || *step <= 0) {
    step = default_step;
  }
  if (integral) {
    step = std::max(1.0, std::round(*step));
  }
  double scaled = *step * scale;
  if (type == u"time" || type == u"datetime-local") {
    scaled = std::max(1.0, std::round(scaled));
  }
  return scaled;
}

bool IsRadioValueMissing(const Element& radio) {
  const std::optional<RadioGroup> group = RadioGroupOf(radio);
  if (!group || group->checked) {
    return false;
  }
  return std::any_of(
      group->members.begin(), group->members.end(),
      [](const Element* member) { return HasAttribute(*member, u"required"); });
}

bool IsInputValid(const Element& input) {
  const std::u16string type = InputType(input);
  const std::u16string value = InputValue(input, type).value_or(u"");
  const bool required = HasAttribute(input, u"required");
  if ((IsTextFieldType(type) || IsTemporalType(type)) && required &&
      value.empty()) {
    return false;
  }
  if ((type == u"checkbox" && required && !IsInputChecked(input, type)) ||
      (type == u"radio" && IsRadioValueMissing(input)) ||
      (type == u"file" && required)) {
    return false;
  }
  if (IsTypeMismatch(input, type, value) ||
      IsPatternMismatch(input, type, value)) {
    return false;
  }
  if (!IsSteppableType(type)) {
    return true;
  }
  const std::optional<double> parsed = ParseForType(type, value);
  if (!parsed) {
    return true;
  }
  const StepRange range = StepRangeOf(input, type);
  if (range.reversed ? (*parsed > range.maximum && *parsed < range.minimum)
                     : (*parsed < range.minimum || *parsed > range.maximum)) {
    return false;
  }
  // A step mismatch needs a base, which is the minimum: the base of an
  // untouched input without one is its own value.
  const std::optional<double> minimum = ParsedAttribute(input, type, u"min");
  const std::optional<double> step = StepOf(input, type);
  if (!minimum || !step) {
    return true;
  }
  const double steps = (*parsed - *minimum) / *step;
  const double tolerance =
      (type == u"number" || type == u"range") ? std::pow(2.0, -24) : 0;
  return std::abs(steps - std::round(steps)) <= tolerance;
}

// Whether a candidate for constraint validation is valid.
bool IsCandidateValid(const Element& element) {
  if (element.HasHTMLTagName(u"input")) {
    return IsInputValid(element);
  }
  if (element.HasHTMLTagName(u"textarea")) {
    return !(HasAttribute(element, u"required") &&
             TextAreaValue(element).empty());
  }
  if (element.HasHTMLTagName(u"select")) {
    return !IsSelectValueMissing(element);
  }
  return true;
}

// Whether a fieldset holds, or a form owns, an invalid candidate.
bool HasInvalidControl(const Element& container) {
  const bool is_form = container.HasHTMLTagName(u"form");
  const Node& root =
      is_form ? container.TreeRoot() : static_cast<const Node&>(container);
  for (const Node* node = root.FirstChild(); node;
       node = node->NextInTree(&root)) {
    if (!node->IsElementNode()) {
      continue;
    }
    const auto& element = static_cast<const Element&>(*node);
    if (!IsFormControl(element) ||
        (is_form && FormOwner(element) != &container)) {
      continue;
    }
    if (WillValidate(element) && !IsCandidateValid(element)) {
      return true;
    }
  }
  return false;
}

// D8: whether a node is editable or an editing host.
bool IsEditableOrEditingHost(const Node& node) {
  if (node.IsElementNode()) {
    const auto& element = static_cast<const Element&>(node);
    if (element.IsHTMLElement()) {
      if (const std::u16string* value =
              element.FindAttribute(u"contenteditable")) {
        const std::u16string lower = base::ToLowerASCII(*value);
        if (lower.empty() || lower == u"true" || lower == u"plaintext-only") {
          return true;
        }
        if (lower == u"false") {
          return false;
        }
      }
    }
  }
  const Node* const parent = node.ParentNode();
  if (!parent || !IsEditableOrEditingHost(*parent)) {
    return false;
  }
  if (!node.IsElementNode()) {
    return parent->IsElementNode() &&
           static_cast<const Element*>(parent)->IsHTMLElement();
  }
  const auto& element = static_cast<const Element&>(node);
  return element.IsHTMLElement() ||
         IsInNamespace(element, u"svg", html_names::kSvgNamespace) ||
         IsInNamespace(element, u"math", html_names::kMathMlNamespace);
}

// The direction of the first strong character of `text`, if it has one.
std::optional<bool> FirstStrongIsRightToLeft(std::u16string_view text) {
  for (size_t i = 0; i < text.size();) {
    const char16_t lead = text[i++];
    UChar32 c = lead;
    if (U16_IS_LEAD(lead) && i < text.size() && U16_IS_TRAIL(text[i])) {
      c = U16_GET_SUPPLEMENTARY(lead, text[i]);
      ++i;
    }
    switch (u_charDirection(c)) {
      case U_LEFT_TO_RIGHT:
        return false;
      case U_RIGHT_TO_LEFT:
      case U_RIGHT_TO_LEFT_ARABIC:
        return true;
      default:
        break;
    }
  }
  return std::nullopt;
}

bool HasValidDir(const Element& element) {
  const std::u16string* dir = element.FindAttribute(u"dir");
  return dir && (base::EqualsCaseInsensitiveASCII(*dir, u"ltr") ||
                 base::EqualsCaseInsensitiveASCII(*dir, u"rtl") ||
                 base::EqualsCaseInsensitiveASCII(*dir, u"auto"));
}

// An element's auto directionality.
bool AutoDirectionIsRightToLeft(const Element& element) {
  if (element.HasHTMLTagName(u"textarea")) {
    return FirstStrongIsRightToLeft(TextAreaValue(element)).value_or(false);
  }
  if (element.HasHTMLTagName(u"input")) {
    const std::u16string type = InputType(element);
    if (IsOneOf(type,
                {u"text", u"search", u"tel", u"url", u"email", u"password",
                 u"submit", u"reset", u"button", u"hidden"})) {
      return FirstStrongIsRightToLeft(InputValue(element, type).value_or(u""))
          .value_or(false);
    }
  }
  for (const Node* node = element.FirstChild(); node;) {
    if (node->IsElementNode()) {
      const auto& child = static_cast<const Element&>(*node);
      if (child.HasHTMLTagName(u"bdi") || child.HasHTMLTagName(u"script") ||
          child.HasHTMLTagName(u"style") || child.HasHTMLTagName(u"textarea") ||
          (child.IsHTMLElement() && HasValidDir(child))) {
        node = node->NextInTreeSkippingChildren(&element);
        continue;
      }
    } else if (node->IsTextNode() ||
               node->GetNodeType() == Node::kCdataSectionNode) {
      if (const std::optional<bool> rtl = FirstStrongIsRightToLeft(
              static_cast<const CharacterData*>(node)->Data())) {
        return *rtl;
      }
    }
    node = node->NextInTree(&element);
  }
  return false;
}

// The language of the document's last content-language meta, or null. A
// Page does not see the response's Content-Language.
std::optional<std::u16string> DocumentContentLanguage(
    const Document& document) {
  std::optional<std::u16string> language;
  for (const Node* node = document.FirstChild(); node;
       node = node->NextInTree(&document)) {
    if (!IsHTML(node, u"meta")) {
      continue;
    }
    const auto& meta = static_cast<const Element&>(*node);
    const std::u16string* http_equiv = meta.FindAttribute(u"http-equiv");
    const std::u16string* content = meta.FindAttribute(u"content");
    if (http_equiv && content &&
        base::EqualsCaseInsensitiveASCII(*http_equiv, u"content-language")) {
      language = *content;
    }
  }
  return language;
}

bool IsValidCustomElementName(std::u16string_view name) {
  if (name.empty() || !base::IsAsciiLower(name.front()) ||
      name.find('-') == std::u16string_view::npos) {
    return false;
  }
  for (const char16_t c : name) {
    if (base::IsAsciiUpper(c) || c == 0 || IsHTMLSpace(c) || c == '/' ||
        c == '>') {
      return false;
    }
  }
  return !IsOneOf(name,
                  {u"annotation-xml", u"color-profile", u"font-face",
                   u"font-face-src", u"font-face-uri", u"font-face-format",
                   u"font-face-name", u"missing-glyph"});
}

}  // namespace

bool IsLink(const Element& element) {
  if (element.HasHTMLTagName(u"a") || element.HasHTMLTagName(u"area")) {
    return HasAttribute(element, u"href");
  }
  if (element.HasHTMLTagName(u"img")) {
    return HasAttribute(element, u"usemap");
  }
  if (IsInNamespace(element, u"a", html_names::kSvgNamespace)) {
    return HasAttribute(element, u"href") ||
           element.FindAttribute(QualifiedName{
               .local_name = u"href",
               .namespace_uri = std::u16string(html_names::kXlinkNamespace)});
  }
  return false;
}

bool MatchesEnabled(const Element& element) {
  if (IsFormControl(element) || element.HasHTMLTagName(u"fieldset") ||
      element.HasHTMLTagName(u"option") ||
      element.HasHTMLTagName(u"optgroup")) {
    return !MatchesDisabled(element);
  }
  return false;
}

bool MatchesDisabled(const Element& element) {
  if (IsFormControl(element) || element.HasHTMLTagName(u"fieldset")) {
    return IsActuallyDisabled(element);
  }
  const bool is_option = element.HasHTMLTagName(u"option");
  if (!is_option && !element.HasHTMLTagName(u"optgroup")) {
    return false;
  }
  if (HasAttribute(element, u"disabled")) {
    return true;
  }
  const SelectParts parts = RelatedSelectParts(element);
  return (is_option && parts.optgroup &&
          HasAttribute(*parts.optgroup, u"disabled")) ||
         (parts.select && IsActuallyDisabled(*parts.select));
}

bool IsChecked(const Element& element) {
  if (element.HasHTMLTagName(u"input")) {
    return IsInputChecked(element, InputType(element));
  }
  return element.HasHTMLTagName(u"option") && IsOptionSelected(element);
}

bool IsDefault(const Element& element) {
  if (element.HasHTMLTagName(u"input")) {
    const std::u16string type = InputType(element);
    if (type == u"checkbox" || type == u"radio") {
      return HasAttribute(element, u"checked");
    }
    return (type == u"submit" || type == u"image") && IsDefaultButton(element);
  }
  if (element.HasHTMLTagName(u"button")) {
    return IsDefaultButton(element);
  }
  return element.HasHTMLTagName(u"option") &&
         HasAttribute(element, u"selected");
}

bool IsIndeterminate(const Element& element) {
  if (element.HasHTMLTagName(u"input") && InputType(element) == u"radio") {
    const std::optional<RadioGroup> group = RadioGroupOf(element);
    return group ? !group->checked : !HasAttribute(element, u"checked");
  }
  return element.HasHTMLTagName(u"progress") &&
         !HasAttribute(element, u"value");
}

bool IsRequired(const Element& element) {
  if (element.HasHTMLTagName(u"input")) {
    return SupportsRequired(InputType(element)) &&
           HasAttribute(element, u"required");
  }
  return (element.HasHTMLTagName(u"select") ||
          element.HasHTMLTagName(u"textarea")) &&
         HasAttribute(element, u"required");
}

bool IsOptional(const Element& element) {
  if (element.HasHTMLTagName(u"input") || element.HasHTMLTagName(u"select") ||
      element.HasHTMLTagName(u"textarea")) {
    return !IsRequired(element);
  }
  return element.HasHTMLTagName(u"button");
}

bool IsReadWrite(const Element& element) {
  if (element.HasHTMLTagName(u"input")) {
    const std::u16string type = InputType(element);
    return (IsTextFieldType(type) || IsTemporalType(type)) &&
           !IsActuallyDisabled(element) && !HasAttribute(element, u"readonly");
  }
  if (element.HasHTMLTagName(u"textarea")) {
    return !IsActuallyDisabled(element) && !HasAttribute(element, u"readonly");
  }
  return element.IsHTMLElement() && IsEditableOrEditingHost(element);
}

bool IsReadOnly(const Element& element) {
  return element.IsHTMLElement() && !IsReadWrite(element);
}

bool IsPlaceholderShown(const Element& element) {
  if (!HasAttribute(element, u"placeholder")) {
    return false;
  }
  if (element.HasHTMLTagName(u"textarea")) {
    return TextAreaValue(element).empty();
  }
  if (!element.HasHTMLTagName(u"input")) {
    return false;
  }
  const std::u16string type = InputType(element);
  return IsTextFieldType(type) &&
         InputValue(element, type).value_or(u"").empty();
}

bool IsValid(const Element& element) {
  if (element.HasHTMLTagName(u"fieldset") || element.HasHTMLTagName(u"form")) {
    return !HasInvalidControl(element);
  }
  return IsFormControl(element) && WillValidate(element) &&
         IsCandidateValid(element);
}

bool IsInvalid(const Element& element) {
  if (element.HasHTMLTagName(u"fieldset") || element.HasHTMLTagName(u"form")) {
    return HasInvalidControl(element);
  }
  return IsFormControl(element) && WillValidate(element) &&
         !IsCandidateValid(element);
}

bool IsInRange(const Element& element) {
  if (!element.HasHTMLTagName(u"input") || !WillValidate(element)) {
    return false;
  }
  const std::u16string type = InputType(element);
  if (!IsSteppableType(type)) {
    return false;
  }
  const std::optional<double> value =
      ParseForType(type, InputValue(element, type).value_or(u""));
  if (!value) {
    return true;
  }
  const StepRange range = StepRangeOf(element, type);
  if (range.reversed) {
    return *value >= range.minimum || *value <= range.maximum;
  }
  if (!range.has_minimum && !range.has_maximum && type != u"range") {
    return false;
  }
  return range.minimum <= *value && *value <= range.maximum;
}

bool IsOutOfRange(const Element& element) {
  if (!element.HasHTMLTagName(u"input") || !WillValidate(element)) {
    return false;
  }
  const std::u16string type = InputType(element);
  if (!IsSteppableType(type)) {
    return false;
  }
  const std::optional<double> value =
      ParseForType(type, InputValue(element, type).value_or(u""));
  if (!value) {
    return false;
  }
  const StepRange range = StepRangeOf(element, type);
  if (range.reversed) {
    return *value > range.maximum && *value < range.minimum;
  }
  if (!range.has_minimum && !range.has_maximum && type != u"range") {
    return false;
  }
  return *value < range.minimum || *value > range.maximum;
}

bool IsDefined(const Element& element) {
  if (!element.IsHTMLElement()) {
    return true;
  }
  // With no definitions, an element of a custom element name or with an is
  // value stays undefined, as the parser makes it.
  return !IsValidCustomElementName(element.LocalName()) &&
         !HasAttribute(element, u"is");
}

bool IsOpen(const Element& element) {
  return (element.HasHTMLTagName(u"details") ||
          element.HasHTMLTagName(u"dialog")) &&
         HasAttribute(element, u"open");
}

bool IsTextField(const Element& element) {
  return element.HasHTMLTagName(u"input") &&
         IsTextFieldType(InputType(element));
}

bool IsRightToLeft(const Element& element) {
  if (element.IsHTMLElement() && HasValidDir(element)) {
    const std::u16string& dir = *element.FindAttribute(u"dir");
    if (base::EqualsCaseInsensitiveASCII(dir, u"rtl")) {
      return true;
    }
    if (base::EqualsCaseInsensitiveASCII(dir, u"ltr")) {
      return false;
    }
    return AutoDirectionIsRightToLeft(element);
  }
  if (element.HasHTMLTagName(u"bdi")) {
    return AutoDirectionIsRightToLeft(element);
  }
  if (element.HasHTMLTagName(u"input") && InputType(element) == u"tel") {
    return false;
  }
  const Element* const parent = element.ParentElement();
  return parent && IsRightToLeft(*parent);
}

std::u16string InheritedLanguage(const Element& element) {
  const QualifiedName xml_lang{
      .local_name = u"lang",
      .namespace_uri = std::u16string(html_names::kXmlNamespace)};
  for (const Node* node = &element; node; node = node->ParentNode()) {
    if (node->IsDocumentNode()) {
      return DocumentContentLanguage(static_cast<const Document&>(*node))
          .value_or(u"");
    }
    if (!node->IsElementNode()) {
      continue;
    }
    const auto& current = static_cast<const Element&>(*node);
    if (const std::u16string* lang = current.FindAttribute(xml_lang)) {
      return *lang;
    }
    if (current.IsHTMLElement() ||
        current.NamespaceURI() == html_names::kSvgNamespace) {
      if (const std::u16string* lang = current.FindAttribute(u"lang")) {
        return *lang;
      }
    }
  }
  return std::u16string();
}

}  // namespace mitto::pseudo_class_state
