#include "mitto/css/css_tokenizer.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "base/strings/string_number_conversions.h"

namespace mitto {

namespace {

// What the input stream gives at its end, and also for a U+0000 that only
// PeekWithoutReplacement() sees, as in Blink.
constexpr char16_t kEndOfFileMarker = 0;
constexpr char32_t kReplacementCharacter = 0xFFFD;

constexpr bool IsAsciiDigit(char32_t c) {
  return c >= '0' && c <= '9';
}

constexpr bool IsAsciiHexDigit(char32_t c) {
  return IsAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

constexpr bool IsAsciiAlpha(char32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

constexpr int HexValue(char32_t c) {
  if (IsAsciiDigit(c)) {
    return c - '0';
  }
  return (c | 0x20) - 'a' + 10;
}

// HTML's space characters, which Blink takes for CSS's whitespace because it
// does not preprocess \r and \f away.
constexpr bool IsHtmlSpace(char32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

constexpr bool IsCssNewLine(char32_t c) {
  return c == '\r' || c == '\n' || c == '\f';
}

constexpr bool IsLeadingSurrogate(char32_t c) {
  return c >= 0xD800 && c <= 0xDBFF;
}

constexpr bool IsTrailingSurrogate(char32_t c) {
  return c >= 0xDC00 && c <= 0xDFFF;
}

// https://drafts.csswg.org/css-syntax/#name-start-code-point
constexpr bool IsNameStartCodePoint(char32_t c) {
  return IsAsciiAlpha(c) || c == '_' || c >= 0x80;
}

// https://drafts.csswg.org/css-syntax/#name-code-point
constexpr bool IsNameCodePoint(char32_t c) {
  return IsNameStartCodePoint(c) || IsAsciiDigit(c) || c == '-';
}

// https://drafts.csswg.org/css-syntax/#check-if-two-code-points-are-a-valid-escape
constexpr bool TwoCharsAreValidEscape(char32_t first, char32_t second) {
  return first == '\\' && !IsCssNewLine(second);
}

// https://drafts.csswg.org/css-syntax/#non-printable-code-point
constexpr bool IsNonPrintableCodePoint(char32_t c) {
  return c <= 0x8 || c == 0xB || (c >= 0xE && c <= 0x1F) || c == 0x7F;
}

void AppendCodePoint(char32_t c, std::u16string& output) {
  if (c <= 0xFFFF) {
    output.push_back(static_cast<char16_t>(c));
  } else {
    output.push_back(static_cast<char16_t>(0xD7C0 + (c >> 10)));
    output.push_back(static_cast<char16_t>(0xDC00 | (c & 0x3FF)));
  }
}

// Upstream: third_party/blink/renderer/core/css/parser/css_tokenizer_input_stream.h
class InputStream {
 public:
  explicit InputStream(std::u16string_view input) : input_(input) {}

  // The next code unit, with U+0000 and a lone surrogate replaced, or
  // kEndOfFileMarker at the end.
  char16_t NextInputChar() const {
    if (offset_ >= input_.size()) {
      return kEndOfFileMarker;
    }
    const char16_t result = input_[offset_];
    if (!result ||
        (IsLeadingSurrogate(result) &&
         !IsTrailingSurrogate(PeekWithoutReplacement(1))) ||
        (IsTrailingSurrogate(result) &&
         !IsLeadingSurrogate(PeekPreviousCharWithoutReplacement()))) {
      return static_cast<char16_t>(kReplacementCharacter);
    }
    return result;
  }

  char16_t PeekPreviousCharWithoutReplacement() const {
    return offset_ == 0 ? kEndOfFileMarker : input_[offset_ - 1];
  }

  // The code unit `lookahead` after the next, or kEndOfFileMarker past the
  // end, which a U+0000 of the input also gives.
  char16_t PeekWithoutReplacement(size_t lookahead) const {
    if (lookahead >= input_.size() - offset_) {
      return kEndOfFileMarker;
    }
    return input_[offset_ + lookahead];
  }

  void Advance(size_t count = 1) {
    offset_ = std::min(offset_ + count, input_.size());
  }

  void PushBack() { --offset_; }

  template <bool Predicate(char32_t)>
  size_t SkipWhilePredicate(size_t lookahead) const {
    while (offset_ + lookahead < input_.size() &&
           Predicate(input_[offset_ + lookahead])) {
      ++lookahead;
    }
    return lookahead;
  }

  void AdvanceUntilNonWhitespace() {
    offset_ += SkipWhilePredicate<IsHtmlSpace>(0);
  }

  // The number in the next `length` code units, which the tokenizer has found
  // to be one.
  double GetDouble(size_t length) const {
    std::string number;
    number.reserve(length);
    for (size_t i = 0; i < length; ++i) {
      number.push_back(static_cast<char>(input_[offset_ + i]));
    }
    double value = 0;
    // Too large a number gives an infinity, which the token clamps.
    base::StringToDouble(number, &value);
    return value;
  }

  std::u16string_view Range(size_t start, size_t length) const {
    return input_.substr(start, length);
  }

  size_t Offset() const { return offset_; }
  bool AtEnd() const { return offset_ >= input_.size(); }

 private:
  const std::u16string_view input_;
  size_t offset_ = 0;
};

class Tokenizer {
 public:
  explicit Tokenizer(std::u16string_view input) : input_(input) {}

  std::vector<CssToken> Run() {
    std::vector<CssToken> tokens;
    while (true) {
      CssToken token = NextToken();
      token.start = token_start_;
      token.end = input_.Offset();
      const bool end_of_file = token.type == CssTokenType::kEndOfFile;
      tokens.push_back(std::move(token));
      if (end_of_file) {
        return tokens;
      }
    }
  }

 private:
  static CssToken Simple(CssTokenType type,
                         CssBlockType block_type = CssBlockType::kNotBlock) {
    CssToken token;
    token.type = type;
    token.block_type = block_type;
    return token;
  }

  static CssToken Delimiter(char32_t c) {
    CssToken token = Simple(CssTokenType::kDelimiter);
    token.delimiter = c;
    return token;
  }

  static CssToken WithValue(CssTokenType type, std::u16string value) {
    CssToken token = Simple(type);
    token.value = std::move(value);
    return token;
  }

  char16_t Consume() {
    const char16_t current = input_.NextInputChar();
    input_.Advance();
    return current;
  }

  void Reconsume() { input_.PushBack(); }

  bool ConsumeIfNext(char16_t c) {
    if (input_.PeekWithoutReplacement(0) == c) {
      input_.Advance();
      return true;
    }
    return false;
  }

  CssToken BlockStart(CssTokenType type) {
    block_stack_.push_back(type);
    return Simple(type, CssBlockType::kBlockStart);
  }

  CssToken BlockEnd(CssTokenType type, CssTokenType start_type) {
    if (!block_stack_.empty() && block_stack_.back() == start_type) {
      block_stack_.pop_back();
      return Simple(type, CssBlockType::kBlockEnd);
    }
    return Simple(type);
  }

  CssToken NextToken() {
    while (true) {
      token_start_ = input_.Offset();
      const char16_t cc = Consume();
      switch (cc) {
        case 0:
          return Simple(CssTokenType::kEndOfFile);
        case '\t':
        case '\n':
        case '\f':
        case '\r':
        case ' ':
          input_.AdvanceUntilNonWhitespace();
          return Simple(CssTokenType::kWhitespace);
        case '\'':
        case '"':
          return ConsumeStringTokenUntil(cc);
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
          Reconsume();
          return ConsumeNumericToken();
        case '(':
          return BlockStart(CssTokenType::kLeftParenthesis);
        case ')':
          return BlockEnd(CssTokenType::kRightParenthesis,
                          CssTokenType::kLeftParenthesis);
        case '[':
          return BlockStart(CssTokenType::kLeftBracket);
        case ']':
          return BlockEnd(CssTokenType::kRightBracket,
                          CssTokenType::kLeftBracket);
        case '{':
          return BlockStart(CssTokenType::kLeftBrace);
        case '}':
          return BlockEnd(CssTokenType::kRightBrace, CssTokenType::kLeftBrace);
        case '+':
        case '.':
          if (NextCharsAreNumber(cc)) {
            Reconsume();
            return ConsumeNumericToken();
          }
          return Delimiter(cc);
        case '-':
          return HyphenMinus(cc);
        case '*':
          if (ConsumeIfNext('=')) {
            return Simple(CssTokenType::kSubstringMatch);
          }
          return Delimiter('*');
        case '<':
          if (input_.PeekWithoutReplacement(0) == '!' &&
              input_.PeekWithoutReplacement(1) == '-' &&
              input_.PeekWithoutReplacement(2) == '-') {
            input_.Advance(3);
            return Simple(CssTokenType::kCdo);
          }
          return Delimiter('<');
        case ',':
          return Simple(CssTokenType::kComma);
        case '/':
          if (ConsumeIfNext('*')) {
            ConsumeUntilCommentEndFound();
            // Read another token.
            continue;
          }
          return Delimiter(cc);
        case '\\':
          if (TwoCharsAreValidEscape(cc, input_.PeekWithoutReplacement(0))) {
            Reconsume();
            return ConsumeIdentLikeToken();
          }
          return Delimiter(cc);
        case ':':
          return Simple(CssTokenType::kColon);
        case ';':
          return Simple(CssTokenType::kSemicolon);
        case '#':
          return Hash(cc);
        case '^':
          if (ConsumeIfNext('=')) {
            return Simple(CssTokenType::kPrefixMatch);
          }
          return Delimiter('^');
        case '$':
          if (ConsumeIfNext('=')) {
            return Simple(CssTokenType::kSuffixMatch);
          }
          return Delimiter('$');
        case '|':
          if (ConsumeIfNext('=')) {
            return Simple(CssTokenType::kDashMatch);
          }
          if (ConsumeIfNext('|')) {
            return Simple(CssTokenType::kColumn);
          }
          return Delimiter('|');
        case '~':
          if (ConsumeIfNext('=')) {
            return Simple(CssTokenType::kIncludeMatch);
          }
          return Delimiter('~');
        case '@':
          if (NextCharsAreIdentifier()) {
            return WithValue(CssTokenType::kAtKeyword, ConsumeName());
          }
          return Delimiter('@');
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 11:
        case 14:
        case 15:
        case 16:
        case 17:
        case 18:
        case 19:
        case 20:
        case 21:
        case 22:
        case 23:
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
        case 31:
        case '!':
        case '%':
        case '&':
        case '=':
        case '>':
        case '?':
        case '`':
        case 127:
          return Delimiter(cc);
        default:
          // 'u' and 'U' start a unicode-range only where unicode-range
          // descriptors are allowed, which a media query never is.
          Reconsume();
          return ConsumeIdentLikeToken();
      }
    }
  }

  CssToken HyphenMinus(char16_t cc) {
    if (NextCharsAreNumber(cc)) {
      Reconsume();
      return ConsumeNumericToken();
    }
    if (input_.PeekWithoutReplacement(0) == '-' &&
        input_.PeekWithoutReplacement(1) == '>') {
      input_.Advance(2);
      return Simple(CssTokenType::kCdc);
    }
    if (NextCharsAreIdentifier(cc)) {
      Reconsume();
      return ConsumeIdentLikeToken();
    }
    return Delimiter(cc);
  }

  CssToken Hash(char16_t cc) {
    const char16_t next_char = input_.NextInputChar();
    if (IsNameCodePoint(next_char) ||
        TwoCharsAreValidEscape(next_char, input_.PeekWithoutReplacement(1))) {
      const CssHashType type = NextCharsAreIdentifier()
                                   ? CssHashType::kId
                                   : CssHashType::kUnrestricted;
      CssToken token = WithValue(CssTokenType::kHash, ConsumeName());
      token.hash_type = type;
      return token;
    }
    return Delimiter(cc);
  }

  // http://www.w3.org/TR/css3-syntax/#consume-a-number
  CssToken ConsumeNumber() {
    CssNumericValueType type = CssNumericValueType::kInteger;
    CssNumericSign sign = CssNumericSign::kNoSign;
    size_t number_length = 0;

    char16_t next = input_.PeekWithoutReplacement(0);
    if (next == '+') {
      ++number_length;
      sign = CssNumericSign::kPlus;
    } else if (next == '-') {
      ++number_length;
      sign = CssNumericSign::kMinus;
    }

    number_length = input_.SkipWhilePredicate<IsAsciiDigit>(number_length);
    next = input_.PeekWithoutReplacement(number_length);
    if (next == '.' &&
        IsAsciiDigit(input_.PeekWithoutReplacement(number_length + 1))) {
      type = CssNumericValueType::kNumber;
      number_length =
          input_.SkipWhilePredicate<IsAsciiDigit>(number_length + 2);
      next = input_.PeekWithoutReplacement(number_length);
    }

    if (next == 'E' || next == 'e') {
      next = input_.PeekWithoutReplacement(number_length + 1);
      if (IsAsciiDigit(next)) {
        type = CssNumericValueType::kNumber;
        number_length =
            input_.SkipWhilePredicate<IsAsciiDigit>(number_length + 1);
      } else if ((next == '+' || next == '-') &&
                 IsAsciiDigit(
                     input_.PeekWithoutReplacement(number_length + 2))) {
        type = CssNumericValueType::kNumber;
        number_length =
            input_.SkipWhilePredicate<IsAsciiDigit>(number_length + 3);
      }
    }

    // A leading '+' is left to the sign, as the number itself is unsigned.
    const size_t sign_length = sign == CssNumericSign::kPlus ? 1 : 0;
    input_.Advance(sign_length);
    const double value = input_.GetDouble(number_length - sign_length);
    input_.Advance(number_length - sign_length);

    CssToken token = Simple(CssTokenType::kNumber);
    // Blink holds a number between the largest floats either way.
    token.numeric_value = std::clamp(
        value, -static_cast<double>(std::numeric_limits<float>::max()),
        static_cast<double>(std::numeric_limits<float>::max()));
    token.numeric_value_type = type;
    token.numeric_sign = sign;
    token.unit = CssUnit::kNumber;
    return token;
  }

  // http://www.w3.org/TR/css3-syntax/#consume-a-numeric-token
  CssToken ConsumeNumericToken() {
    CssToken token = ConsumeNumber();
    if (NextCharsAreIdentifier()) {
      token.type = CssTokenType::kDimension;
      token.value = ConsumeName();
      token.unit = CssUnitFromName(token.value);
    } else if (ConsumeIfNext('%')) {
      token.type = CssTokenType::kPercentage;
      token.unit = CssUnit::kPercentage;
    }
    return token;
  }

  // https://drafts.csswg.org/css-syntax/#consume-ident-like-token
  CssToken ConsumeIdentLikeToken() {
    std::u16string name = ConsumeName();
    if (ConsumeIfNext('(')) {
      if (name.size() == 3 && (name[0] | 0x20) == 'u' &&
          (name[1] | 0x20) == 'r' && (name[2] | 0x20) == 'l') {
        // The spec is slightly different so as to avoid dropping whitespace
        // tokens, but they would not be used, as in Blink.
        input_.AdvanceUntilNonWhitespace();
        const char16_t next = input_.PeekWithoutReplacement(0);
        if (next != '"' && next != '\'') {
          return ConsumeUrlToken();
        }
      }
      block_stack_.push_back(CssTokenType::kLeftParenthesis);
      CssToken token = WithValue(CssTokenType::kFunction, std::move(name));
      token.block_type = CssBlockType::kBlockStart;
      return token;
    }
    return WithValue(CssTokenType::kIdent, std::move(name));
  }

  // https://drafts.csswg.org/css-syntax/#consume-a-string-token
  CssToken ConsumeStringTokenUntil(char16_t ending_code_point) {
    // A string without escapes or U+0000 is the input as it is, lone
    // surrogates and all, as in Blink.
    for (size_t size = 0;; ++size) {
      const char16_t cc = input_.PeekWithoutReplacement(size);
      if (cc == ending_code_point) {
        const size_t start = input_.Offset();
        input_.Advance(size + 1);
        return WithValue(CssTokenType::kString,
                         std::u16string(input_.Range(start, size)));
      }
      if (IsCssNewLine(cc)) {
        input_.Advance(size);
        return Simple(CssTokenType::kBadString);
      }
      if (cc == kEndOfFileMarker || cc == '\\') {
        break;
      }
    }

    std::u16string output;
    while (true) {
      const char16_t cc = Consume();
      if (cc == ending_code_point || cc == kEndOfFileMarker) {
        return WithValue(CssTokenType::kString, std::move(output));
      }
      if (IsCssNewLine(cc)) {
        Reconsume();
        return Simple(CssTokenType::kBadString);
      }
      if (cc == '\\') {
        if (input_.NextInputChar() == kEndOfFileMarker) {
          continue;
        }
        if (IsCssNewLine(input_.PeekWithoutReplacement(0))) {
          // This handles \r\n too.
          ConsumeSingleWhitespaceIfNext();
        } else {
          AppendCodePoint(ConsumeEscape(), output);
        }
      } else {
        output.push_back(cc);
      }
    }
  }

  // https://drafts.csswg.org/css-syntax/#consume-url-token
  CssToken ConsumeUrlToken() {
    input_.AdvanceUntilNonWhitespace();

    // A URL without escapes is the input as it is, as in Blink.
    for (size_t size = 0;; ++size) {
      const char16_t cc = input_.PeekWithoutReplacement(size);
      if (cc == ')') {
        const size_t start = input_.Offset();
        input_.Advance(size + 1);
        return WithValue(CssTokenType::kUrl,
                         std::u16string(input_.Range(start, size)));
      }
      if (cc <= ' ' || cc == '\\' || cc == '"' || cc == '\'' || cc == '(' ||
          cc == 0x7F) {
        break;
      }
    }

    std::u16string result;
    while (true) {
      const char16_t cc = Consume();
      if (cc == ')' || cc == kEndOfFileMarker) {
        return WithValue(CssTokenType::kUrl, std::move(result));
      }
      if (IsHtmlSpace(cc)) {
        input_.AdvanceUntilNonWhitespace();
        if (ConsumeIfNext(')') || input_.NextInputChar() == kEndOfFileMarker) {
          return WithValue(CssTokenType::kUrl, std::move(result));
        }
        break;
      }
      if (cc == '"' || cc == '\'' || cc == '(' || IsNonPrintableCodePoint(cc)) {
        break;
      }
      if (cc == '\\') {
        if (TwoCharsAreValidEscape(cc, input_.PeekWithoutReplacement(0))) {
          AppendCodePoint(ConsumeEscape(), result);
          continue;
        }
        break;
      }
      result.push_back(cc);
    }
    ConsumeBadUrlRemnants();
    return Simple(CssTokenType::kBadUrl);
  }

  // https://drafts.csswg.org/css-syntax/#consume-the-remnants-of-a-bad-url
  void ConsumeBadUrlRemnants() {
    while (true) {
      const char16_t cc = Consume();
      if (cc == ')' || cc == kEndOfFileMarker) {
        return;
      }
      if (TwoCharsAreValidEscape(cc, input_.PeekWithoutReplacement(0))) {
        ConsumeEscape();
      }
    }
  }

  void ConsumeSingleWhitespaceIfNext() {
    // \r\n and HTML's spaces, since nothing preprocessed them.
    const char16_t next = input_.PeekWithoutReplacement(0);
    if (next == '\r' && input_.PeekWithoutReplacement(1) == '\n') {
      input_.Advance(2);
    } else if (IsHtmlSpace(next)) {
      input_.Advance();
    }
  }

  void ConsumeUntilCommentEndFound() {
    char16_t c = Consume();
    while (true) {
      if (c == kEndOfFileMarker) {
        return;
      }
      if (c != '*') {
        c = Consume();
        continue;
      }
      c = Consume();
      if (c == '/') {
        return;
      }
    }
  }

  // http://www.w3.org/TR/css3-syntax/#consume-a-name
  std::u16string ConsumeName() {
    std::u16string result;
    while (true) {
      const char16_t cc = input_.NextInputChar();
      if (IsNameCodePoint(cc)) {
        input_.Advance();
        result.push_back(cc);
      } else if (TwoCharsAreValidEscape(cc, input_.PeekWithoutReplacement(1))) {
        input_.Advance();
        AppendCodePoint(ConsumeEscape(), result);
      } else {
        return result;
      }
    }
  }

  // https://drafts.csswg.org/css-syntax/#consume-an-escaped-code-point
  char32_t ConsumeEscape() {
    char16_t cc = input_.NextInputChar();
    input_.Advance();
    if (IsAsciiHexDigit(cc)) {
      size_t consumed_hex_digits = 1;
      uint32_t code_point = HexValue(cc);
      while (consumed_hex_digits < 6 &&
             IsAsciiHexDigit(input_.PeekWithoutReplacement(0))) {
        cc = input_.NextInputChar();
        input_.Advance();
        code_point = code_point * 16 + HexValue(cc);
        ++consumed_hex_digits;
      }
      ConsumeSingleWhitespaceIfNext();
      if (code_point == 0 || (code_point >= 0xD800 && code_point <= 0xDFFF) ||
          code_point > 0x10FFFF) {
        return kReplacementCharacter;
      }
      return code_point;
    }
    if (cc == kEndOfFileMarker) {
      return kReplacementCharacter;
    }
    return cc;
  }

  // http://www.w3.org/TR/css3-syntax/#starts-with-a-number
  bool NextCharsAreNumber(char16_t first) const {
    const char16_t second = input_.PeekWithoutReplacement(0);
    if (IsAsciiDigit(first)) {
      return true;
    }
    if (first == '+' || first == '-') {
      return IsAsciiDigit(second) ||
             (second == '.' && IsAsciiDigit(input_.PeekWithoutReplacement(1)));
    }
    if (first == '.') {
      return IsAsciiDigit(second);
    }
    return false;
  }

  // https://drafts.csswg.org/css-syntax/#would-start-an-identifier
  bool NextCharsAreIdentifier(char16_t first) const {
    const char16_t second = input_.NextInputChar();
    if (IsNameStartCodePoint(first) || TwoCharsAreValidEscape(first, second)) {
      return true;
    }
    if (first == '-') {
      return IsNameStartCodePoint(second) || second == '-' ||
             TwoCharsAreValidEscape(second, input_.PeekWithoutReplacement(1));
    }
    return false;
  }

  bool NextCharsAreIdentifier() {
    if (input_.AtEnd()) {
      return false;
    }
    const char16_t first = Consume();
    const bool are_identifier = NextCharsAreIdentifier(first);
    Reconsume();
    return are_identifier;
  }

  InputStream input_;
  std::vector<CssTokenType> block_stack_;
  size_t token_start_ = 0;
};

}  // namespace

std::vector<CssToken> TokenizeCss(std::u16string_view input) {
  return Tokenizer(input).Run();
}

}  // namespace mitto
