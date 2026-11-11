#include "mitto/dom/selector_query.h"

#include <stdint.h>

#include <algorithm>
#include <string_view>
#include <utility>

#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"

namespace mitto {

namespace {

bool IsWhitespace(char16_t c) {
  // CSS's whitespace, after the preprocessing that turns CR and FF into LF.
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool IsHexDigit(char16_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool IsNameStart(char16_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         c >= 0x80;
}

bool IsNameCharacter(char16_t c) {
  return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-';
}

// Whether a value is an attribute whose value the HTML Standard says matches
// in any case in an HTML document, in its "case-sensitivity of selectors".
bool IsCaseInsensitiveAttribute(std::u16string_view name) {
  constexpr std::u16string_view kNames[] = {
      u"accept",    u"accept-charset", u"align",     u"alink",    u"axis",
      u"bgcolor",   u"charset",        u"checked",   u"clear",    u"codetype",
      u"color",     u"compact",        u"declare",   u"defer",    u"dir",
      u"direction", u"disabled",       u"enctype",   u"face",     u"frame",
      u"hreflang",  u"http-equiv",     u"lang",      u"language", u"link",
      u"media",     u"method",         u"multiple",  u"nohref",   u"noresize",
      u"noshade",   u"nowrap",         u"readonly",  u"rel",      u"rev",
      u"rules",     u"scope",          u"scrolling", u"selected", u"shape",
      u"target",    u"text",           u"type",      u"valign",   u"valuetype",
      u"vlink"};
  return std::find(std::begin(kNames), std::end(kNames), name) !=
         std::end(kNames);
}

bool EqualsInCase(std::u16string_view a,
                  std::u16string_view b,
                  bool case_insensitive) {
  return case_insensitive ? base::EqualsCaseInsensitiveASCII(a, b) : a == b;
}

// A parser of selectors, on the characters of the text, which tokenizes as
// CSS's tokenizer does as it goes.
class Parser {
 public:
  explicit Parser(std::u16string_view text) : text_(Preprocess(text)) {}

  enum class Result {
    kValid,
    kInvalid,
    // Valid, and needs what the Runtime lacks.
    kUnsupported,
  };

  Result ParseList(std::vector<SelectorQuery::Complex>& list) {
    while (true) {
      SkipWhitespace();
      SelectorQuery::Complex complex;
      const Result result = ParseComplex(complex);
      if (result != Result::kValid) {
        return result;
      }
      list.push_back(std::move(complex));
      SkipWhitespace();
      if (AtEnd()) {
        return Result::kValid;
      }
      if (Peek() != ',') {
        return Result::kInvalid;
      }
      ++position_;
    }
  }

 private:
  bool AtEnd() const { return position_ >= text_.size(); }
  char16_t Peek(size_t offset = 0) const {
    return position_ + offset < text_.size() ? text_[position_ + offset] : 0;
  }
  // CSS's tokenizer consumes comments between tokens and makes no token of
  // them, so a comment is not whitespace: "div/**/p" is two names with
  // nothing between them.
  void SkipComments() {
    while (Peek() == '/' && Peek(1) == '*') {
      position_ += 2;
      while (!AtEnd() && !(Peek() == '*' && Peek(1) == '/')) {
        ++position_;
      }
      // A comment that the input ends in is closed there.
      position_ = std::min(position_ + 2, text_.size());
    }
  }

  // Skips whitespace and comments, and returns whether there was whitespace.
  bool SkipWhitespace() {
    bool whitespace = false;
    while (true) {
      SkipComments();
      if (AtEnd() || !IsWhitespace(Peek())) {
        return whitespace;
      }
      whitespace = true;
      ++position_;
    }
  }

  // Whether a backslash at `offset` starts a valid escape: one not followed by
  // a newline.
  bool IsValidEscape(size_t offset) const {
    return Peek(offset) == '\\' && Peek(offset + 1) != '\n';
  }

  bool StartsIdentifier(size_t offset = 0) const {
    const char16_t first = Peek(offset);
    if (first == '-') {
      return IsNameStart(Peek(offset + 1)) || Peek(offset + 1) == '-' ||
             IsValidEscape(offset + 1);
    }
    return IsNameStart(first) || IsValidEscape(offset);
  }

  // CSS's "consume an escaped code point", after the backslash.
  void ConsumeEscape(std::u16string& out) {
    if (AtEnd()) {
      out += u'�';
      return;
    }
    if (!IsHexDigit(Peek())) {
      out += Peek();
      ++position_;
      return;
    }
    uint32_t code_point = 0;
    for (int digits = 0; digits < 6 && IsHexDigit(Peek()); ++digits) {
      const char16_t c = Peek();
      code_point =
          code_point * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
      ++position_;
    }
    if (IsWhitespace(Peek())) {
      ++position_;
    }
    if (code_point == 0 || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point <= 0xDFFF)) {
      code_point = 0xFFFD;
    }
    if (code_point >= 0x10000) {
      code_point -= 0x10000;
      out += static_cast<char16_t>(0xD800 + (code_point >> 10));
      out += static_cast<char16_t>(0xDC00 + (code_point & 0x3FF));
    } else {
      out += static_cast<char16_t>(code_point);
    }
  }

  // CSS's "consume an ident sequence".
  std::u16string ConsumeName() {
    std::u16string name;
    while (!AtEnd()) {
      if (IsNameCharacter(Peek())) {
        name += Peek();
        ++position_;
      } else if (IsValidEscape(0)) {
        ++position_;
        ConsumeEscape(name);
      } else {
        break;
      }
    }
    return name;
  }

  bool ConsumeIdentifier(std::u16string& name) {
    if (!StartsIdentifier()) {
      return false;
    }
    name = ConsumeName();
    return true;
  }

  // A string token, from its quote on. Returns false for a bad string.
  bool ConsumeString(std::u16string& value) {
    const char16_t quote = Peek();
    ++position_;
    while (!AtEnd()) {
      const char16_t c = Peek();
      if (c == quote) {
        ++position_;
        return true;
      }
      if (c == '\n') {
        return false;
      }
      if (c == '\\') {
        if (Peek(1) == '\n') {
          position_ += 2;
          continue;
        }
        ++position_;
        ConsumeEscape(value);
        continue;
      }
      value += c;
      ++position_;
    }
    // A string the input ends in is closed there.
    return true;
  }

  Result ParseComplex(SelectorQuery::Complex& complex) {
    SelectorQuery::Combinator combinator = SelectorQuery::Combinator::kNone;
    while (true) {
      SelectorQuery::Compound compound;
      compound.combinator = combinator;
      const Result result = ParseCompound(compound);
      if (result != Result::kValid) {
        return result;
      }
      complex.push_back(std::move(compound));
      const bool had_whitespace = SkipWhitespace();
      if (AtEnd() || Peek() == ',') {
        return Result::kValid;
      }
      switch (Peek()) {
        case '>':
          combinator = SelectorQuery::Combinator::kChild;
          ++position_;
          break;
        case '+':
          combinator = SelectorQuery::Combinator::kNextSibling;
          ++position_;
          break;
        case '~':
          combinator = SelectorQuery::Combinator::kSubsequentSibling;
          ++position_;
          break;
        default:
          if (!had_whitespace) {
            return Result::kInvalid;
          }
          combinator = SelectorQuery::Combinator::kDescendant;
      }
      SkipWhitespace();
    }
  }

  Result ParseCompound(SelectorQuery::Compound& compound) {
    std::u16string name;
    SkipComments();
    if (Peek() == '*') {
      ++position_;
      compound.simples.push_back({SelectorQuery::Simple::Kind::kUniversal});
    } else if (ConsumeIdentifier(name)) {
      compound.simples.push_back(
          {SelectorQuery::Simple::Kind::kType, std::move(name)});
    }
    if (Peek() == '|') {
      // A namespace prefix, which needs @namespace rules.
      return Result::kUnsupported;
    }
    while (true) {
      SkipComments();
      if (AtEnd()) {
        break;
      }
      const char16_t c = Peek();
      if (c == '#') {
        ++position_;
        std::u16string id;
        if (!ConsumeIdentifier(id)) {
          return Result::kInvalid;
        }
        compound.simples.push_back(
            {SelectorQuery::Simple::Kind::kId, std::move(id)});
      } else if (c == '.') {
        ++position_;
        std::u16string class_name;
        if (!ConsumeIdentifier(class_name)) {
          return Result::kInvalid;
        }
        compound.simples.push_back(
            {SelectorQuery::Simple::Kind::kClass, std::move(class_name)});
      } else if (c == '[') {
        ++position_;
        const Result result = ParseAttribute(compound);
        if (result != Result::kValid) {
          return result;
        }
      } else if (c == ':') {
        // A pseudo-class or a pseudo-element. Each needs a table of its own
        // and most of them style, so all are Known Gaps for now, valid or not.
        return Result::kUnsupported;
      } else {
        break;
      }
    }
    return compound.simples.empty() ? Result::kInvalid : Result::kValid;
  }

  Result ParseAttribute(SelectorQuery::Compound& compound) {
    SkipWhitespace();
    if (Peek() == '|' || Peek() == '*') {
      return Result::kUnsupported;
    }
    SelectorQuery::Simple simple{SelectorQuery::Simple::Kind::kAttributeExists};
    if (!ConsumeIdentifier(simple.name)) {
      return Result::kInvalid;
    }
    if (Peek() == '|' && Peek(1) != '=') {
      return Result::kUnsupported;
    }
    SkipWhitespace();
    if (Peek() == ']') {
      ++position_;
      compound.simples.push_back(std::move(simple));
      return Result::kValid;
    }
    const char16_t matcher = Peek();
    if (matcher == '=') {
      simple.kind = SelectorQuery::Simple::Kind::kAttributeEquals;
      ++position_;
    } else if (Peek(1) == '=') {
      switch (matcher) {
        case '~':
          simple.kind = SelectorQuery::Simple::Kind::kAttributeListContains;
          break;
        case '|':
          simple.kind = SelectorQuery::Simple::Kind::kAttributeDashMatch;
          break;
        case '^':
          simple.kind = SelectorQuery::Simple::Kind::kAttributeBegins;
          break;
        case '$':
          simple.kind = SelectorQuery::Simple::Kind::kAttributeEnds;
          break;
        case '*':
          simple.kind = SelectorQuery::Simple::Kind::kAttributeContains;
          break;
        default:
          return Result::kInvalid;
      }
      position_ += 2;
    } else {
      return Result::kInvalid;
    }
    SkipWhitespace();
    if (Peek() == '"' || Peek() == '\'') {
      if (!ConsumeString(simple.value)) {
        return Result::kInvalid;
      }
    } else if (!ConsumeIdentifier(simple.value)) {
      return Result::kInvalid;
    }
    SkipWhitespace();
    std::u16string flag;
    if (ConsumeIdentifier(flag)) {
      // Blink takes the flag i, and not yet s.
      if (!base::EqualsCaseInsensitiveASCII(flag, u"i")) {
        return Result::kInvalid;
      }
      simple.value_case = SelectorQuery::Simple::Case::kInsensitive;
      SkipWhitespace();
    }
    if (Peek() != ']') {
      return Result::kInvalid;
    }
    ++position_;
    compound.simples.push_back(std::move(simple));
    return Result::kValid;
  }

  // CSS Syntax's input preprocessing: CR LF, CR and FF become LF, and U+0000
  // becomes U+FFFD, before anything is tokenized.
  static std::u16string Preprocess(std::u16string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
      const char16_t c = text[i];
      if (c == '\r') {
        result += u'\n';
        if (i + 1 < text.size() && text[i + 1] == '\n') {
          ++i;
        }
      } else if (c == '\f') {
        result += u'\n';
      } else if (c == 0) {
        result += u'\uFFFD';
      } else {
        result += c;
      }
    }
    return result;
  }

  const std::u16string text_;
  size_t position_ = 0;
};

bool ClassListContains(std::u16string_view classes,
                       std::u16string_view name,
                       bool case_insensitive) {
  size_t start = 0;
  while (start < classes.size()) {
    while (start < classes.size() && IsWhitespace(classes[start])) {
      ++start;
    }
    size_t end = start;
    while (end < classes.size() && !IsWhitespace(classes[end])) {
      ++end;
    }
    if (end > start && EqualsInCase(classes.substr(start, end - start), name,
                                    case_insensitive)) {
      return true;
    }
    start = end;
  }
  return false;
}

bool MatchesSimple(const SelectorQuery::Simple& simple,
                   const Element& element) {
  const bool in_html =
      element.IsHTMLElement() && element.GetDocument().IsHTMLDocument();
  const bool quirks = element.GetDocument().InQuirksMode();
  using Kind = SelectorQuery::Simple::Kind;
  switch (simple.kind) {
    case Kind::kUniversal:
      return true;
    case Kind::kType:
      return in_html ? element.LocalName() == base::ToLowerASCII(simple.name)
                     : element.LocalName() == simple.name;
    case Kind::kId: {
      const std::u16string* id = element.FindAttribute(u"id");
      return id && EqualsInCase(*id, simple.name, quirks);
    }
    case Kind::kClass: {
      const std::u16string* classes = element.FindAttribute(u"class");
      return classes && ClassListContains(*classes, simple.name, quirks);
    }
    default:
      break;
  }
  const std::u16string name =
      in_html ? base::ToLowerASCII(simple.name) : simple.name;
  const std::u16string* value = element.FindAttribute(name);
  if (!value) {
    return false;
  }
  bool insensitive = false;
  switch (simple.value_case) {
    case SelectorQuery::Simple::Case::kInsensitive:
      insensitive = true;
      break;
    case SelectorQuery::Simple::Case::kDefault:
      insensitive = in_html && IsCaseInsensitiveAttribute(name);
      break;
  }
  const std::u16string_view actual = *value;
  const std::u16string_view expected = simple.value;
  switch (simple.kind) {
    case Kind::kAttributeExists:
      return true;
    case Kind::kAttributeEquals:
      return EqualsInCase(actual, expected, insensitive);
    case Kind::kAttributeListContains:
      if (expected.empty() ||
          std::any_of(expected.begin(), expected.end(), IsWhitespace)) {
        return false;
      }
      return ClassListContains(actual, expected, insensitive);
    case Kind::kAttributeDashMatch:
      return EqualsInCase(actual, expected, insensitive) ||
             (actual.size() > expected.size() &&
              actual[expected.size()] == '-' &&
              EqualsInCase(actual.substr(0, expected.size()), expected,
                           insensitive));
    case Kind::kAttributeBegins:
      return !expected.empty() && actual.size() >= expected.size() &&
             EqualsInCase(actual.substr(0, expected.size()), expected,
                          insensitive);
    case Kind::kAttributeEnds:
      return !expected.empty() && actual.size() >= expected.size() &&
             EqualsInCase(actual.substr(actual.size() - expected.size()),
                          expected, insensitive);
    case Kind::kAttributeContains:
      if (expected.empty() || actual.size() < expected.size()) {
        return false;
      }
      for (size_t i = 0; i + expected.size() <= actual.size(); ++i) {
        if (EqualsInCase(actual.substr(i, expected.size()), expected,
                         insensitive)) {
          return true;
        }
      }
      return false;
    default:
      return false;
  }
}

bool MatchesCompound(const SelectorQuery::Compound& compound,
                     const Element& element) {
  return std::all_of(compound.simples.begin(), compound.simples.end(),
                     [&element](const SelectorQuery::Simple& simple) {
                       return MatchesSimple(simple, element);
                     });
}

// Matches the compounds of `complex` up to `index` against `element`, from
// right to left, as SelectorChecker does.
bool MatchesComplex(const SelectorQuery::Complex& complex,
                    size_t index,
                    const Element& element) {
  if (!MatchesCompound(complex[index], element)) {
    return false;
  }
  if (index == 0) {
    return true;
  }
  using Combinator = SelectorQuery::Combinator;
  switch (complex[index].combinator) {
    case Combinator::kDescendant:
      for (const Element* ancestor = element.ParentElement(); ancestor;
           ancestor = ancestor->ParentElement()) {
        if (MatchesComplex(complex, index - 1, *ancestor)) {
          return true;
        }
      }
      return false;
    case Combinator::kChild: {
      const Element* const parent = element.ParentElement();
      return parent && MatchesComplex(complex, index - 1, *parent);
    }
    case Combinator::kNextSibling: {
      const Element* const previous = element.PreviousElementSibling();
      return previous && MatchesComplex(complex, index - 1, *previous);
    }
    case Combinator::kSubsequentSibling:
      for (const Element* previous = element.PreviousElementSibling(); previous;
           previous = previous->PreviousElementSibling()) {
        if (MatchesComplex(complex, index - 1, *previous)) {
          return true;
        }
      }
      return false;
    case Combinator::kNone:
      return false;
  }
  NOTREACHED();
}

}  // namespace

// static
std::optional<SelectorQuery> SelectorQuery::Parse(
    const std::u16string& selectors,
    bindings::ExceptionState& exception_state) {
  if (selectors.empty()) {
    exception_state.ThrowDOMException("SyntaxError",
                                      "The provided selector is empty.");
    return std::nullopt;
  }
  std::vector<Complex> list;
  switch (Parser(selectors).ParseList(list)) {
    case Parser::Result::kValid:
      return SelectorQuery(std::move(list));
    case Parser::Result::kInvalid:
      exception_state.ThrowDOMException(
          "SyntaxError", base::StrCat({"'", base::UTF16ToUTF8(selectors),
                                       "' is not a valid selector."}));
      return std::nullopt;
    case Parser::Result::kUnsupported:
      exception_state.ThrowTypeError("Not implemented.");
      return std::nullopt;
  }
  NOTREACHED();
}

SelectorQuery::SelectorQuery(std::vector<Complex> list)
    : list_(std::move(list)) {}

SelectorQuery::SelectorQuery(SelectorQuery&&) = default;
SelectorQuery& SelectorQuery::operator=(SelectorQuery&&) = default;
SelectorQuery::~SelectorQuery() = default;

bool SelectorQuery::Matches(const Element& element) const {
  return std::any_of(
      list_.begin(), list_.end(), [&element](const Complex& complex) {
        return MatchesComplex(complex, complex.size() - 1, element);
      });
}

Element* SelectorQuery::QueryFirst(const Node& root) const {
  for (Node* node = root.FirstChild(); node; node = node->NextInTree(&root)) {
    if (node->IsElementNode() && Matches(static_cast<Element&>(*node))) {
      return static_cast<Element*>(node);
    }
  }
  return nullptr;
}

}  // namespace mitto
