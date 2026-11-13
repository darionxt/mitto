#include "mitto/html/html_tokenizer.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "mitto/html/html_entity_table.h"

namespace mitto {

namespace {

using State = HtmlTokenizer::State;
using Code = HtmlParseErrorCode;

// Outside Unicode, so no input character is it.
constexpr char32_t kEndOfFile = 0xFFFFFFFF;
constexpr char32_t kReplacementCharacter = 0xFFFD;
constexpr char32_t kMaxCodePoint = 0x10FFFF;

// Where a tag has so many attributes that a set finds a duplicate sooner
// than comparing each name with every other.
constexpr size_t kMaxAttributesToCompareOneByOne = 16;

constexpr bool IsAsciiUpperAlpha(char32_t c) {
  return c >= 'A' && c <= 'Z';
}

constexpr bool IsAsciiAlpha(char32_t c) {
  return IsAsciiUpperAlpha(c) || (c >= 'a' && c <= 'z');
}

constexpr bool IsAsciiDigit(char32_t c) {
  return c >= '0' && c <= '9';
}

constexpr bool IsAsciiAlphanumeric(char32_t c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c);
}

constexpr bool IsAsciiHexDigit(char32_t c) {
  return IsAsciiDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

constexpr uint32_t HexDigitValue(char32_t c) {
  if (IsAsciiDigit(c)) {
    return c - '0';
  }
  return (c | 0x20) - 'a' + 10;
}

constexpr char32_t ToAsciiLower(char32_t c) {
  return IsAsciiUpperAlpha(c) ? c + ('a' - 'A') : c;
}

// The whitespace that the states look for. A CR never reaches them, because
// the input stream has made it LF.
constexpr bool IsWhitespace(char32_t c) {
  return c == '\t' || c == '\n' || c == '\f' || c == ' ';
}

constexpr bool IsAsciiWhitespace(char32_t c) {
  return IsWhitespace(c) || c == '\r';
}

constexpr bool IsLeadSurrogate(char32_t c) {
  return c >= 0xD800 && c <= 0xDBFF;
}

constexpr bool IsTrailSurrogate(char32_t c) {
  return c >= 0xDC00 && c <= 0xDFFF;
}

constexpr bool IsSurrogate(char32_t c) {
  return c >= 0xD800 && c <= 0xDFFF;
}

constexpr bool IsNoncharacter(char32_t c) {
  return (c >= 0xFDD0 && c <= 0xFDEF) ||
         (c <= kMaxCodePoint && (c & 0xFFFE) == 0xFFFE);
}

constexpr bool IsControl(char32_t c) {
  return c <= 0x1F || (c >= 0x7F && c <= 0x9F);
}

void AppendCodePoint(std::u16string& string, char32_t c) {
  if (c <= 0xFFFF) {
    string.push_back(static_cast<char16_t>(c));
    return;
  }
  c -= 0x10000;
  string.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
  string.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
}

bool EqualsIgnoringAsciiCase(std::u16string_view string,
                             std::u16string_view lower_case) {
  return std::ranges::equal(string, lower_case, [](char16_t a, char16_t b) {
    return ToAsciiLower(a) == char32_t{b};
  });
}

// What a numeric character reference from 0x80 to 0x9F stands for, as in
// windows-1252, or 0 where it stands for itself.
constexpr std::array<char16_t, 32> kC1Replacements = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// Narrows the table of named character references to the names that start
// with what it has been given, one character at a time, as Blink's
// HTMLEntitySearch does.
class EntitySearch {
 public:
  EntitySearch() : end_(HtmlEntityTable().size()) {}

  // Returns false once no name starts with what it has been given.
  bool Advance(char16_t c) {
    const base::span<const HtmlEntity> table = HtmlEntityTable();
    // The name that is all of what came before sorts first, and is too short.
    if (begin_ < end_ && table[begin_].name.size() == length_) {
      ++begin_;
    }
    const base::span<const HtmlEntity> range =
        table.subspan(begin_, end_ - begin_);
    const auto [first, last] = std::ranges::equal_range(
        range, c, {}, [this](const HtmlEntity& entity) {
          return static_cast<char16_t>(
              static_cast<unsigned char>(entity.name[length_]));
        });
    end_ = begin_ + static_cast<size_t>(last - range.begin());
    begin_ += static_cast<size_t>(first - range.begin());
    ++length_;
    return begin_ < end_;
  }

  // The name that is exactly what it has been given, if there is one.
  const HtmlEntity* match() const {
    if (begin_ == end_ || HtmlEntityTable()[begin_].name.size() != length_) {
      return nullptr;
    }
    return &HtmlEntityTable()[begin_];
  }

  // Whether a longer name starts with what it has been given.
  bool CanExtend() const {
    return begin_ < end_ && HtmlEntityTable()[end_ - 1].name.size() > length_;
  }

 private:
  // The names that start with what it has been given, in the table.
  size_t begin_ = 0;
  size_t end_;
  size_t length_ = 0;
};

bool IsCharacterReferenceState(State state) {
  switch (state) {
    case State::kCharacterReference:
    case State::kNamedCharacterReference:
    case State::kAmbiguousAmpersand:
    case State::kNumericCharacterReference:
    case State::kHexadecimalCharacterReferenceStart:
    case State::kHexadecimalCharacterReference:
    case State::kDecimalCharacterReference:
    case State::kNumericCharacterReferenceEnd:
      return true;
    default:
      return false;
  }
}

}  // namespace

HtmlTokenizer::HtmlTokenizer() : HtmlTokenizer(Options()) {}

HtmlTokenizer::HtmlTokenizer(const Options& options) : options_(options) {}

HtmlTokenizer::~HtmlTokenizer() = default;

void HtmlTokenizer::Append(std::u16string_view input) {
  DCHECK(!finished_);
  // What is used up goes, once it is half of what is kept.
  if (input_position_ > 0 && input_position_ >= input_.size() / 2) {
    input_.erase(0, input_position_);
    input_position_ = 0;
  }
  // The standard's preprocessing of the input stream: CR LF and CR become LF.
  // A CR at the end of one piece and an LF at the start of the next are one.
  input_.reserve(input_.size() + input.size());
  for (const char16_t unit : input) {
    if (unit == '\n' && last_input_was_cr_) {
      last_input_was_cr_ = false;
      continue;
    }
    last_input_was_cr_ = unit == '\r';
    input_.push_back(last_input_was_cr_ ? u'\n' : unit);
  }
}

void HtmlTokenizer::Finish() {
  finished_ = true;
}

bool HtmlTokenizer::NextToken(HtmlToken& token) {
  while (tokens_.empty()) {
    if (emitted_end_of_file_) {
      return false;
    }
    if (!Step()) {
      FlushCharacters();
      if (tokens_.empty()) {
        return false;
      }
    }
  }
  token = std::move(tokens_.front());
  tokens_.pop_front();
  return true;
}

void HtmlTokenizer::SetState(State state) {
  DCHECK(!reconsume_);
  DCHECK(state == State::kData || state == State::kRcdata ||
         state == State::kRawtext || state == State::kScriptData ||
         state == State::kPlaintext || state == State::kCdataSection);
  state_ = state;
}

void HtmlTokenizer::SetLastStartTagName(std::u16string_view name) {
  last_start_tag_name_ = name;
}

std::vector<HtmlTokenizer::ParseError> HtmlTokenizer::TakeParseErrors() {
  return std::exchange(errors_, {});
}

bool HtmlTokenizer::Consume() {
  if (input_position_ == input_.size()) {
    if (!finished_) {
      return false;
    }
    if (c_ == kEndOfFile) {
      // Already there.
      c_was_null_ = false;
      return true;
    }
    c_ = kEndOfFile;
  } else {
    char32_t c = input_[input_position_];
    size_t length = 1;
    if (IsLeadSurrogate(c)) {
      if (input_position_ + 1 < input_.size()) {
        const char32_t trail = input_[input_position_ + 1];
        if (IsTrailSurrogate(trail)) {
          c = 0x10000 + ((c - 0xD800) << 10) + (trail - 0xDC00);
          length = 2;
        }
      } else if (!finished_) {
        // Its trail surrogate may come in the next piece.
        return false;
      }
    }
    input_position_ += length;
    c_ = c;
  }
  c_was_null_ = false;
  if (current_is_line_feed_) {
    ++line_;
    column_ = 1;
  } else {
    column_ += current_length_;
  }
  current_is_line_feed_ = c_ == '\n';
  current_length_ = c_ > 0xFFFF && c_ != kEndOfFile ? 2 : 1;

  // The parse errors of the standard's preprocessing of the input stream.
  if (c_ == kEndOfFile) {
    return true;
  }
  if (IsSurrogate(c_)) {
    ReportError(Code::kSurrogateInInputStream);
  } else if (IsNoncharacter(c_)) {
    ReportError(Code::kNoncharacterInInputStream);
  } else if (c_ != 0 && IsControl(c_) && !IsAsciiWhitespace(c_)) {
    ReportError(Code::kControlCharacterInInputStream);
  }
  return true;
}

HtmlTokenizer::Peek HtmlTokenizer::PeekUnit(size_t offset,
                                            char16_t& unit) const {
  const size_t index = input_position_ + offset;
  if (index < input_.size()) {
    unit = input_[index];
    return Peek::kCharacter;
  }
  return finished_ ? Peek::kEndOfFile : Peek::kNeedMoreInput;
}

HtmlTokenizer::LookAheadResult HtmlTokenizer::LookAhead(
    std::u16string_view expected,
    bool ignore_case) const {
  const auto same = [ignore_case](char32_t c, char16_t expected_unit) {
    return (ignore_case ? ToAsciiLower(c) : c) == char32_t{expected_unit};
  };
  if (!same(c_, expected[0])) {
    return LookAheadResult::kNoMatch;
  }
  for (size_t i = 1; i < expected.size(); ++i) {
    char16_t unit;
    switch (PeekUnit(i - 1, unit)) {
      case Peek::kCharacter:
        if (!same(unit, expected[i])) {
          return LookAheadResult::kNoMatch;
        }
        break;
      case Peek::kEndOfFile:
        return LookAheadResult::kNoMatch;
      case Peek::kNeedMoreInput:
        return LookAheadResult::kNeedMoreInput;
    }
  }
  return LookAheadResult::kMatch;
}

void HtmlTokenizer::ConsumeAscii(size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const bool consumed = Consume();
    DCHECK(consumed);
    DCHECK_LT(c_, 0x80u);
  }
}

HtmlTokenizer::Position HtmlTokenizer::CurrentPosition() const {
  return {line_, column_};
}

HtmlTokenizer::Position HtmlTokenizer::NextPosition() const {
  if (current_is_line_feed_) {
    return {line_ + 1, 1};
  }
  return {line_, column_ + current_length_};
}

HtmlTokenizer::ZeroBasedPosition HtmlTokenizer::TextPosition() const {
  const Position next = NextPosition();
  return {next.line - 1, next.column - 1};
}

void HtmlTokenizer::ReportError(HtmlParseErrorCode code) {
  ReportErrorAt(code, CurrentPosition());
}

void HtmlTokenizer::ReportErrorAt(HtmlParseErrorCode code,
                                  const Position& position) {
  if (options_.collect_parse_errors) {
    errors_.push_back({code, position.line, position.column});
  }
}

void HtmlTokenizer::EmitCharacter(char32_t c) {
  AppendCodePoint(characters_, c);
}

void HtmlTokenizer::EmitCharacters(std::u16string_view characters) {
  characters_.append(characters);
}

void HtmlTokenizer::EmitCurrentToken() {
  if (current_token_.type == HtmlToken::Type::kStartTag) {
    DropAttributeIfDuplicate();
    last_start_tag_name_ = current_token_.name;
  } else if (current_token_.type == HtmlToken::Type::kEndTag) {
    DropAttributeIfDuplicate();
    if (!current_token_.attributes.empty()) {
      ReportError(Code::kEndTagWithAttributes);
    }
    if (current_token_.self_closing) {
      ReportError(Code::kEndTagWithTrailingSolidus);
    }
  }
  FlushCharacters();
  tokens_.push_back(std::exchange(current_token_, HtmlToken()));
}

void HtmlTokenizer::EmitEndOfFile() {
  FlushCharacters();
  HtmlToken token;
  token.type = HtmlToken::Type::kEndOfFile;
  tokens_.push_back(std::move(token));
  emitted_end_of_file_ = true;
}

void HtmlTokenizer::FlushCharacters() {
  if (characters_.empty()) {
    return;
  }
  HtmlToken token;
  token.type = HtmlToken::Type::kCharacter;
  token.data = std::exchange(characters_, std::u16string());
  tokens_.push_back(std::move(token));
}

void HtmlTokenizer::CreateStartTag() {
  current_token_ = HtmlToken();
  current_token_.type = HtmlToken::Type::kStartTag;
  current_attribute_is_duplicate_ = false;
  attribute_names_.clear();
}

void HtmlTokenizer::CreateEndTag() {
  CreateStartTag();
  current_token_.type = HtmlToken::Type::kEndTag;
}

void HtmlTokenizer::CreateComment(std::u16string_view data) {
  current_token_ = HtmlToken();
  current_token_.type = HtmlToken::Type::kComment;
  current_token_.data = data;
}

void HtmlTokenizer::CreateDoctype() {
  current_token_ = HtmlToken();
  current_token_.type = HtmlToken::Type::kDoctype;
  current_token_.name_missing = true;
}

void HtmlTokenizer::StartAttribute() {
  DropAttributeIfDuplicate();
  current_token_.attributes.emplace_back();
}

void HtmlTokenizer::FinishAttributeName() {
  // The standard removes an attribute whose name an earlier one of the tag
  // has, when its name ends. Its value is read and dropped with it.
  std::vector<HtmlAttribute>& attributes = current_token_.attributes;
  const std::u16string& name = attributes.back().name;
  bool duplicate = false;
  if (attributes.size() <= kMaxAttributesToCompareOneByOne) {
    duplicate = std::ranges::any_of(
        base::span(attributes).first(attributes.size() - 1),
        [&name](const HtmlAttribute& other) { return other.name == name; });
  } else {
    if (attribute_names_.empty()) {
      for (const HtmlAttribute& other :
           base::span(attributes).first(attributes.size() - 1)) {
        attribute_names_.insert(other.name);
      }
    }
    duplicate = !attribute_names_.insert(name).second;
  }
  if (duplicate) {
    ReportError(Code::kDuplicateAttribute);
    current_attribute_is_duplicate_ = true;
  }
}

void HtmlTokenizer::DropAttributeIfDuplicate() {
  if (current_attribute_is_duplicate_) {
    current_token_.attributes.pop_back();
    current_attribute_is_duplicate_ = false;
  }
}

bool HtmlTokenizer::IsAppropriateEndTag() const {
  return !last_start_tag_name_.empty() &&
         current_token_.name == last_start_tag_name_;
}

bool HtmlTokenizer::IsReturnStateAttributeValue() const {
  return return_state_ == State::kAttributeValueDoubleQuoted ||
         return_state_ == State::kAttributeValueSingleQuoted ||
         return_state_ == State::kAttributeValueUnquoted;
}

void HtmlTokenizer::FlushCharacterReference() {
  if (IsReturnStateAttributeValue()) {
    current_token_.attributes.back().value.append(temporary_buffer_);
  } else {
    EmitCharacters(temporary_buffer_);
  }
}

void HtmlTokenizer::ConvertTemporaryBufferToComment() {
  CreateComment(u"?");
  current_token_.data.append(temporary_buffer_);
}

bool HtmlTokenizer::Step() {
  if (reconsume_) {
    reconsume_ = false;
  } else if (!Consume()) {
    return false;
  }

  // Blink's InputStreamPreprocessor deals with U+0000 before a state sees it,
  // by the state that reads it, where the standard leaves it to each state.
  // Its entity parser reads the character reference states' input as it is,
  // so there U+0000 goes on to the state they return to.
  if (c_ == 0 && !IsCharacterReferenceState(state_)) {
    if (!force_null_character_replacement_ &&
        (state_ == State::kData || state_ == State::kRcdata ||
         state_ == State::kRawtext)) {
      // Blink drops it, where the standard emits it in the data state for the
      // tree builder to ignore, and U+FFFD in the other two.
      ReportError(Code::kUnexpectedNullCharacter);
      return true;
    }
    // Everywhere else Blink makes it U+FFFD, also where the standard emits it
    // as it is, in the data state and a CDATA section, or hands it on, from
    // the tag open state to the data state.
    c_ = kReplacementCharacter;
    c_was_null_ = true;
  }

  const char32_t c = c_;
  switch (state_) {
    case State::kData:
      if (c == '&') {
        return_state_ = State::kData;
        state_ = State::kCharacterReference;
      } else if (c == '<') {
        state_ = State::kTagOpen;
      } else if (c == kEndOfFile) {
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          // U+FFFD, as above, where the standard emits U+0000.
          ReportError(Code::kUnexpectedNullCharacter);
        }
        EmitCharacter(c);
      }
      break;

    case State::kRcdata:
      if (c == '&') {
        return_state_ = State::kRcdata;
        state_ = State::kCharacterReference;
      } else if (c == '<') {
        state_ = State::kRcdataLessThanSign;
      } else if (c == kEndOfFile) {
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        EmitCharacter(c);
      }
      break;

    case State::kRawtext:
      if (c == '<') {
        state_ = State::kRawtextLessThanSign;
      } else if (c == kEndOfFile) {
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        EmitCharacter(c);
      }
      break;

    case State::kScriptData:
      if (c == '<') {
        state_ = State::kScriptDataLessThanSign;
      } else if (c == kEndOfFile) {
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        EmitCharacter(c);
      }
      break;

    case State::kPlaintext:
      if (c == kEndOfFile) {
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        EmitCharacter(c);
      }
      break;

    case State::kTagOpen:
      if (c == '!') {
        state_ = State::kMarkupDeclarationOpen;
      } else if (c == '/') {
        state_ = State::kEndTagOpen;
      } else if (IsAsciiAlpha(c)) {
        CreateStartTag();
        Reconsume(State::kTagName);
      } else if (c == '?') {
        if (options_.processing_instructions) {
          temporary_buffer_.clear();
          state_ = State::kProcessingInstructionOpen;
        } else {
          ReportError(Code::kUnexpectedQuestionMarkInsteadOfTagName);
          CreateComment(u"");
          Reconsume(State::kBogusComment);
        }
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofBeforeTagName);
        EmitCharacter('<');
        EmitEndOfFile();
      } else {
        ReportError(Code::kInvalidFirstCharacterOfTagName);
        EmitCharacter('<');
        Reconsume(State::kData);
      }
      break;

    case State::kEndTagOpen:
      if (IsAsciiAlpha(c)) {
        CreateEndTag();
        Reconsume(State::kTagName);
      } else if (c == '>') {
        ReportError(Code::kMissingEndTagName);
        state_ = State::kData;
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofBeforeTagName);
        EmitCharacters(u"</");
        EmitEndOfFile();
      } else {
        ReportError(Code::kInvalidFirstCharacterOfTagName);
        CreateComment(u"");
        Reconsume(State::kBogusComment);
      }
      break;

    case State::kTagName:
      if (IsWhitespace(c)) {
        state_ = State::kBeforeAttributeName;
      } else if (c == '/') {
        state_ = State::kSelfClosingStartTag;
      } else if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInTag);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        AppendCodePoint(current_token_.name, ToAsciiLower(c));
      }
      break;

    case State::kRcdataLessThanSign:
      if (c == '/') {
        temporary_buffer_.clear();
        state_ = State::kRcdataEndTagOpen;
      } else {
        EmitCharacter('<');
        Reconsume(State::kRcdata);
      }
      break;

    case State::kRcdataEndTagOpen:
      if (IsAsciiAlpha(c)) {
        CreateEndTag();
        Reconsume(State::kRcdataEndTagName);
      } else {
        EmitCharacters(u"</");
        Reconsume(State::kRcdata);
      }
      break;

    case State::kRcdataEndTagName:
      StepEndTagName(c, State::kRcdata);
      break;

    case State::kRawtextLessThanSign:
      if (c == '/') {
        temporary_buffer_.clear();
        state_ = State::kRawtextEndTagOpen;
      } else {
        EmitCharacter('<');
        Reconsume(State::kRawtext);
      }
      break;

    case State::kRawtextEndTagOpen:
      if (IsAsciiAlpha(c)) {
        CreateEndTag();
        Reconsume(State::kRawtextEndTagName);
      } else {
        EmitCharacters(u"</");
        Reconsume(State::kRawtext);
      }
      break;

    case State::kRawtextEndTagName:
      StepEndTagName(c, State::kRawtext);
      break;

    case State::kScriptDataLessThanSign:
      if (c == '/') {
        temporary_buffer_.clear();
        state_ = State::kScriptDataEndTagOpen;
      } else if (c == '!') {
        state_ = State::kScriptDataEscapeStart;
        EmitCharacters(u"<!");
      } else {
        EmitCharacter('<');
        Reconsume(State::kScriptData);
      }
      break;

    case State::kScriptDataEndTagOpen:
      if (IsAsciiAlpha(c)) {
        CreateEndTag();
        Reconsume(State::kScriptDataEndTagName);
      } else {
        EmitCharacters(u"</");
        Reconsume(State::kScriptData);
      }
      break;

    case State::kScriptDataEndTagName:
      StepEndTagName(c, State::kScriptData);
      break;

    case State::kScriptDataEscapeStart:
      if (c == '-') {
        state_ = State::kScriptDataEscapeStartDash;
        EmitCharacter('-');
      } else {
        Reconsume(State::kScriptData);
      }
      break;

    case State::kScriptDataEscapeStartDash:
      if (c == '-') {
        state_ = State::kScriptDataEscapedDashDash;
        EmitCharacter('-');
      } else {
        Reconsume(State::kScriptData);
      }
      break;

    case State::kScriptDataEscaped:
      if (c == '-') {
        state_ = State::kScriptDataEscapedDash;
        EmitCharacter('-');
      } else if (c == '<') {
        state_ = State::kScriptDataEscapedLessThanSign;
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInScriptHtmlCommentLikeText);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        EmitCharacter(c);
      }
      break;

    case State::kScriptDataEscapedDash:
      if (c == '-') {
        state_ = State::kScriptDataEscapedDashDash;
        EmitCharacter('-');
      } else if (c == '<') {
        state_ = State::kScriptDataEscapedLessThanSign;
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInScriptHtmlCommentLikeText);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        state_ = State::kScriptDataEscaped;
        EmitCharacter(c);
      }
      break;

    case State::kScriptDataEscapedDashDash:
      if (c == '-') {
        EmitCharacter('-');
      } else if (c == '<') {
        state_ = State::kScriptDataEscapedLessThanSign;
      } else if (c == '>') {
        state_ = State::kScriptData;
        EmitCharacter('>');
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInScriptHtmlCommentLikeText);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        state_ = State::kScriptDataEscaped;
        EmitCharacter(c);
      }
      break;

    case State::kScriptDataEscapedLessThanSign:
      if (c == '/') {
        temporary_buffer_.clear();
        state_ = State::kScriptDataEscapedEndTagOpen;
      } else if (IsAsciiAlpha(c)) {
        temporary_buffer_.clear();
        EmitCharacter('<');
        Reconsume(State::kScriptDataDoubleEscapeStart);
      } else {
        EmitCharacter('<');
        Reconsume(State::kScriptDataEscaped);
      }
      break;

    case State::kScriptDataEscapedEndTagOpen:
      if (IsAsciiAlpha(c)) {
        CreateEndTag();
        Reconsume(State::kScriptDataEscapedEndTagName);
      } else {
        EmitCharacters(u"</");
        Reconsume(State::kScriptDataEscaped);
      }
      break;

    case State::kScriptDataEscapedEndTagName:
      StepEndTagName(c, State::kScriptDataEscaped);
      break;

    case State::kScriptDataDoubleEscapeStart:
      if (IsWhitespace(c) || c == '/' || c == '>') {
        state_ = temporary_buffer_ == u"script"
                     ? State::kScriptDataDoubleEscaped
                     : State::kScriptDataEscaped;
        EmitCharacter(c);
      } else if (IsAsciiAlpha(c)) {
        AppendCodePoint(temporary_buffer_, ToAsciiLower(c));
        EmitCharacter(c);
      } else {
        Reconsume(State::kScriptDataEscaped);
      }
      break;

    case State::kScriptDataDoubleEscaped:
      if (c == '-') {
        state_ = State::kScriptDataDoubleEscapedDash;
        EmitCharacter('-');
      } else if (c == '<') {
        state_ = State::kScriptDataDoubleEscapedLessThanSign;
        EmitCharacter('<');
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInScriptHtmlCommentLikeText);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        EmitCharacter(c);
      }
      break;

    case State::kScriptDataDoubleEscapedDash:
      if (c == '-') {
        state_ = State::kScriptDataDoubleEscapedDashDash;
        EmitCharacter('-');
      } else if (c == '<') {
        state_ = State::kScriptDataDoubleEscapedLessThanSign;
        EmitCharacter('<');
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInScriptHtmlCommentLikeText);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        state_ = State::kScriptDataDoubleEscaped;
        EmitCharacter(c);
      }
      break;

    case State::kScriptDataDoubleEscapedDashDash:
      if (c == '-') {
        EmitCharacter('-');
      } else if (c == '<') {
        state_ = State::kScriptDataDoubleEscapedLessThanSign;
        EmitCharacter('<');
      } else if (c == '>') {
        state_ = State::kScriptData;
        EmitCharacter('>');
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInScriptHtmlCommentLikeText);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        state_ = State::kScriptDataDoubleEscaped;
        EmitCharacter(c);
      }
      break;

    case State::kScriptDataDoubleEscapedLessThanSign:
      if (c == '/') {
        temporary_buffer_.clear();
        state_ = State::kScriptDataDoubleEscapeEnd;
        EmitCharacter('/');
      } else {
        Reconsume(State::kScriptDataDoubleEscaped);
      }
      break;

    case State::kScriptDataDoubleEscapeEnd:
      if (IsWhitespace(c) || c == '/' || c == '>') {
        state_ = temporary_buffer_ == u"script"
                     ? State::kScriptDataEscaped
                     : State::kScriptDataDoubleEscaped;
        EmitCharacter(c);
      } else if (IsAsciiAlpha(c)) {
        AppendCodePoint(temporary_buffer_, ToAsciiLower(c));
        EmitCharacter(c);
      } else {
        Reconsume(State::kScriptDataDoubleEscaped);
      }
      break;

    case State::kBeforeAttributeName:
      if (IsWhitespace(c)) {
        break;
      }
      if (c == '/' || c == '>' || c == kEndOfFile) {
        Reconsume(State::kAfterAttributeName);
      } else if (c == '=') {
        ReportError(Code::kUnexpectedEqualsSignBeforeAttributeName);
        StartAttribute();
        AppendCodePoint(current_token_.attributes.back().name, c);
        state_ = State::kAttributeName;
      } else {
        StartAttribute();
        Reconsume(State::kAttributeName);
      }
      break;

    case State::kAttributeName:
      if (IsWhitespace(c) || c == '/' || c == '>' || c == kEndOfFile) {
        FinishAttributeName();
        Reconsume(State::kAfterAttributeName);
      } else if (c == '=') {
        FinishAttributeName();
        state_ = State::kBeforeAttributeValue;
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        } else if (c == '"' || c == '\'' || c == '<') {
          ReportError(Code::kUnexpectedCharacterInAttributeName);
        }
        AppendCodePoint(current_token_.attributes.back().name, ToAsciiLower(c));
      }
      break;

    case State::kAfterAttributeName:
      if (IsWhitespace(c)) {
        break;
      }
      if (c == '/') {
        state_ = State::kSelfClosingStartTag;
      } else if (c == '=') {
        state_ = State::kBeforeAttributeValue;
      } else if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInTag);
        EmitEndOfFile();
      } else {
        StartAttribute();
        Reconsume(State::kAttributeName);
      }
      break;

    case State::kBeforeAttributeValue:
      if (IsWhitespace(c)) {
        break;
      }
      if (c == '"') {
        state_ = State::kAttributeValueDoubleQuoted;
      } else if (c == '\'') {
        state_ = State::kAttributeValueSingleQuoted;
      } else if (c == '>') {
        ReportError(Code::kMissingAttributeValue);
        state_ = State::kData;
        EmitCurrentToken();
      } else {
        Reconsume(State::kAttributeValueUnquoted);
      }
      break;

    case State::kAttributeValueDoubleQuoted:
    case State::kAttributeValueSingleQuoted:
      if (c == (state_ == State::kAttributeValueDoubleQuoted ? '"' : '\'')) {
        state_ = State::kAfterAttributeValueQuoted;
      } else if (c == '&') {
        return_state_ = state_;
        state_ = State::kCharacterReference;
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInTag);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        AppendCodePoint(current_token_.attributes.back().value, c);
      }
      break;

    case State::kAttributeValueUnquoted:
      if (IsWhitespace(c)) {
        state_ = State::kBeforeAttributeName;
      } else if (c == '&') {
        return_state_ = State::kAttributeValueUnquoted;
        state_ = State::kCharacterReference;
      } else if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInTag);
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        } else if (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`') {
          ReportError(Code::kUnexpectedCharacterInUnquotedAttributeValue);
        }
        AppendCodePoint(current_token_.attributes.back().value, c);
      }
      break;

    case State::kAfterAttributeValueQuoted:
      if (IsWhitespace(c)) {
        state_ = State::kBeforeAttributeName;
      } else if (c == '/') {
        state_ = State::kSelfClosingStartTag;
      } else if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInTag);
        EmitEndOfFile();
      } else {
        ReportError(Code::kMissingWhitespaceBetweenAttributes);
        Reconsume(State::kBeforeAttributeName);
      }
      break;

    case State::kSelfClosingStartTag:
      if (c == '>') {
        current_token_.self_closing = true;
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInTag);
        EmitEndOfFile();
      } else {
        ReportError(Code::kUnexpectedSolidusInTag);
        Reconsume(State::kBeforeAttributeName);
      }
      break;

    case State::kBogusComment:
      if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        AppendCodePoint(current_token_.data, c);
      }
      break;

    case State::kMarkupDeclarationOpen: {
      // The standard looks at the next characters without consuming the
      // current one. Here the current one is the first of them.
      LookAheadResult result = LookAheadResult::kNoMatch;
      if (c == '-') {
        result = LookAhead(u"--", /*ignore_case=*/false);
        if (result == LookAheadResult::kMatch) {
          ConsumeAscii(1);
          CreateComment(u"");
          state_ = State::kCommentStart;
          break;
        }
      } else if (c == 'D' || c == 'd') {
        result = LookAhead(u"doctype", /*ignore_case=*/true);
        if (result == LookAheadResult::kMatch) {
          ConsumeAscii(6);
          state_ = State::kDoctype;
          break;
        }
      } else if (c == '[') {
        result = LookAhead(u"[CDATA[", /*ignore_case=*/false);
        if (result == LookAheadResult::kMatch) {
          ConsumeAscii(6);
          if (allow_cdata_) {
            state_ = State::kCdataSection;
          } else {
            ReportError(Code::kCdataInHtmlContent);
            CreateComment(u"[CDATA[");
            state_ = State::kBogusComment;
          }
          break;
        }
      }
      if (result == LookAheadResult::kNeedMoreInput) {
        Reconsume(State::kMarkupDeclarationOpen);
        return false;
      }
      ReportError(Code::kIncorrectlyOpenedComment);
      CreateComment(u"");
      Reconsume(State::kBogusComment);
      break;
    }

    case State::kCommentStart:
      if (c == '-') {
        state_ = State::kCommentStartDash;
      } else if (c == '>') {
        ReportError(Code::kAbruptClosingOfEmptyComment);
        state_ = State::kData;
        EmitCurrentToken();
      } else {
        Reconsume(State::kComment);
      }
      break;

    case State::kCommentStartDash:
      if (c == '-') {
        state_ = State::kCommentEnd;
      } else if (c == '>') {
        ReportError(Code::kAbruptClosingOfEmptyComment);
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInComment);
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        current_token_.data.push_back('-');
        Reconsume(State::kComment);
      }
      break;

    case State::kComment:
      if (c == '<') {
        current_token_.data.push_back('<');
        state_ = State::kCommentLessThanSign;
      } else if (c == '-') {
        state_ = State::kCommentEndDash;
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInComment);
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        AppendCodePoint(current_token_.data, c);
      }
      break;

    case State::kCommentLessThanSign:
      if (c == '!') {
        current_token_.data.push_back('!');
        state_ = State::kCommentLessThanSignBang;
      } else if (c == '<') {
        current_token_.data.push_back('<');
      } else {
        Reconsume(State::kComment);
      }
      break;

    case State::kCommentLessThanSignBang:
      if (c == '-') {
        state_ = State::kCommentLessThanSignBangDash;
      } else {
        Reconsume(State::kComment);
      }
      break;

    case State::kCommentLessThanSignBangDash:
      if (c == '-') {
        state_ = State::kCommentLessThanSignBangDashDash;
      } else {
        Reconsume(State::kCommentEndDash);
      }
      break;

    case State::kCommentLessThanSignBangDashDash:
      if (c != '>' && c != kEndOfFile) {
        ReportError(Code::kNestedComment);
      }
      Reconsume(State::kCommentEnd);
      break;

    case State::kCommentEndDash:
      if (c == '-') {
        state_ = State::kCommentEnd;
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInComment);
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        current_token_.data.push_back('-');
        Reconsume(State::kComment);
      }
      break;

    case State::kCommentEnd:
      if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == '!') {
        state_ = State::kCommentEndBang;
      } else if (c == '-') {
        current_token_.data.push_back('-');
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInComment);
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        current_token_.data.append(u"--");
        Reconsume(State::kComment);
      }
      break;

    case State::kCommentEndBang:
      if (c == '-') {
        current_token_.data.append(u"--!");
        state_ = State::kCommentEndDash;
      } else if (c == '>') {
        ReportError(Code::kIncorrectlyClosedComment);
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInComment);
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        current_token_.data.append(u"--!");
        Reconsume(State::kComment);
      }
      break;

    case State::kDoctype:
      if (IsWhitespace(c)) {
        state_ = State::kBeforeDoctypeName;
      } else if (c == '>') {
        Reconsume(State::kBeforeDoctypeName);
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        CreateDoctype();
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        ReportError(Code::kMissingWhitespaceBeforeDoctypeName);
        Reconsume(State::kBeforeDoctypeName);
      }
      break;

    case State::kBeforeDoctypeName:
      if (IsWhitespace(c)) {
        break;
      }
      if (c == '>') {
        ReportError(Code::kMissingDoctypeName);
        CreateDoctype();
        current_token_.force_quirks = true;
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        CreateDoctype();
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        CreateDoctype();
        current_token_.name_missing = false;
        AppendCodePoint(current_token_.name, ToAsciiLower(c));
        state_ = State::kDoctypeName;
      }
      break;

    case State::kDoctypeName:
      if (IsWhitespace(c)) {
        state_ = State::kAfterDoctypeName;
      } else if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        AppendCodePoint(current_token_.name, ToAsciiLower(c));
      }
      break;

    case State::kAfterDoctypeName: {
      if (IsWhitespace(c)) {
        break;
      }
      if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
        break;
      }
      if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
        break;
      }
      const LookAheadResult public_keyword =
          LookAhead(u"public", /*ignore_case=*/true);
      if (public_keyword == LookAheadResult::kMatch) {
        ConsumeAscii(5);
        state_ = State::kAfterDoctypePublicKeyword;
        break;
      }
      const LookAheadResult system_keyword =
          LookAhead(u"system", /*ignore_case=*/true);
      if (system_keyword == LookAheadResult::kMatch) {
        ConsumeAscii(5);
        state_ = State::kAfterDoctypeSystemKeyword;
        break;
      }
      if (public_keyword == LookAheadResult::kNeedMoreInput ||
          system_keyword == LookAheadResult::kNeedMoreInput) {
        Reconsume(State::kAfterDoctypeName);
        return false;
      }
      ReportError(Code::kInvalidCharacterSequenceAfterDoctypeName);
      current_token_.force_quirks = true;
      Reconsume(State::kBogusDoctype);
      break;
    }

    case State::kAfterDoctypePublicKeyword:
    case State::kAfterDoctypeSystemKeyword: {
      const bool is_public = state_ == State::kAfterDoctypePublicKeyword;
      if (IsWhitespace(c)) {
        state_ = is_public ? State::kBeforeDoctypePublicIdentifier
                           : State::kBeforeDoctypeSystemIdentifier;
      } else if (c == '"' || c == '\'') {
        ReportError(is_public
                        ? Code::kMissingWhitespaceAfterDoctypePublicKeyword
                        : Code::kMissingWhitespaceAfterDoctypeSystemKeyword);
        if (is_public) {
          current_token_.public_identifier.emplace();
          state_ = c == '"' ? State::kDoctypePublicIdentifierDoubleQuoted
                            : State::kDoctypePublicIdentifierSingleQuoted;
        } else {
          current_token_.system_identifier.emplace();
          state_ = c == '"' ? State::kDoctypeSystemIdentifierDoubleQuoted
                            : State::kDoctypeSystemIdentifierSingleQuoted;
        }
      } else if (c == '>') {
        ReportError(is_public ? Code::kMissingDoctypePublicIdentifier
                              : Code::kMissingDoctypeSystemIdentifier);
        current_token_.force_quirks = true;
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        ReportError(is_public
                        ? Code::kMissingQuoteBeforeDoctypePublicIdentifier
                        : Code::kMissingQuoteBeforeDoctypeSystemIdentifier);
        current_token_.force_quirks = true;
        Reconsume(State::kBogusDoctype);
      }
      break;
    }

    case State::kBeforeDoctypePublicIdentifier:
    case State::kBeforeDoctypeSystemIdentifier: {
      const bool is_public = state_ == State::kBeforeDoctypePublicIdentifier;
      if (IsWhitespace(c)) {
        break;
      }
      if (c == '"' || c == '\'') {
        if (is_public) {
          current_token_.public_identifier.emplace();
          state_ = c == '"' ? State::kDoctypePublicIdentifierDoubleQuoted
                            : State::kDoctypePublicIdentifierSingleQuoted;
        } else {
          current_token_.system_identifier.emplace();
          state_ = c == '"' ? State::kDoctypeSystemIdentifierDoubleQuoted
                            : State::kDoctypeSystemIdentifierSingleQuoted;
        }
      } else if (c == '>') {
        ReportError(is_public ? Code::kMissingDoctypePublicIdentifier
                              : Code::kMissingDoctypeSystemIdentifier);
        current_token_.force_quirks = true;
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        ReportError(is_public
                        ? Code::kMissingQuoteBeforeDoctypePublicIdentifier
                        : Code::kMissingQuoteBeforeDoctypeSystemIdentifier);
        current_token_.force_quirks = true;
        Reconsume(State::kBogusDoctype);
      }
      break;
    }

    case State::kDoctypePublicIdentifierDoubleQuoted:
    case State::kDoctypePublicIdentifierSingleQuoted:
    case State::kDoctypeSystemIdentifierDoubleQuoted:
    case State::kDoctypeSystemIdentifierSingleQuoted: {
      const bool is_public =
          state_ == State::kDoctypePublicIdentifierDoubleQuoted ||
          state_ == State::kDoctypePublicIdentifierSingleQuoted;
      const bool double_quoted =
          state_ == State::kDoctypePublicIdentifierDoubleQuoted ||
          state_ == State::kDoctypeSystemIdentifierDoubleQuoted;
      if (c == (double_quoted ? '"' : '\'')) {
        state_ = is_public ? State::kAfterDoctypePublicIdentifier
                           : State::kAfterDoctypeSystemIdentifier;
      } else if (c == '>') {
        ReportError(is_public ? Code::kAbruptDoctypePublicIdentifier
                              : Code::kAbruptDoctypeSystemIdentifier);
        current_token_.force_quirks = true;
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        if (c_was_null_) {
          ReportError(Code::kUnexpectedNullCharacter);
        }
        AppendCodePoint(is_public ? *current_token_.public_identifier
                                  : *current_token_.system_identifier,
                        c);
      }
      break;
    }

    case State::kAfterDoctypePublicIdentifier:
    case State::kBetweenDoctypePublicAndSystemIdentifiers: {
      const bool after = state_ == State::kAfterDoctypePublicIdentifier;
      if (IsWhitespace(c)) {
        if (after) {
          state_ = State::kBetweenDoctypePublicAndSystemIdentifiers;
        }
      } else if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == '"' || c == '\'') {
        if (after) {
          ReportError(
              Code::kMissingWhitespaceBetweenDoctypePublicAndSystemIdentifiers);
        }
        current_token_.system_identifier.emplace();
        state_ = c == '"' ? State::kDoctypeSystemIdentifierDoubleQuoted
                          : State::kDoctypeSystemIdentifierSingleQuoted;
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        ReportError(Code::kMissingQuoteBeforeDoctypeSystemIdentifier);
        current_token_.force_quirks = true;
        Reconsume(State::kBogusDoctype);
      }
      break;
    }

    case State::kAfterDoctypeSystemIdentifier:
      if (IsWhitespace(c)) {
        break;
      }
      if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInDoctype);
        current_token_.force_quirks = true;
        EmitCurrentToken();
        EmitEndOfFile();
      } else {
        ReportError(Code::kUnexpectedCharacterAfterDoctypeSystemIdentifier);
        Reconsume(State::kBogusDoctype);
      }
      break;

    case State::kBogusDoctype:
      if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        EmitCurrentToken();
        EmitEndOfFile();
      } else if (c_was_null_) {
        ReportError(Code::kUnexpectedNullCharacter);
      }
      break;

    case State::kCdataSection:
      if (c == ']') {
        state_ = State::kCdataSectionBracket;
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInCdata);
        EmitEndOfFile();
      } else {
        // U+FFFD for U+0000, as above, which the standard emits as it is for
        // the tree builder to replace.
        EmitCharacter(c);
      }
      break;

    case State::kCdataSectionBracket:
      if (c == ']') {
        state_ = State::kCdataSectionEnd;
      } else {
        EmitCharacter(']');
        Reconsume(State::kCdataSection);
      }
      break;

    case State::kCdataSectionEnd:
      if (c == ']') {
        EmitCharacter(']');
      } else if (c == '>') {
        state_ = State::kData;
      } else {
        EmitCharacters(u"]]");
        Reconsume(State::kCdataSection);
      }
      break;

    case State::kCharacterReference:
      temporary_buffer_ = u"&";
      if (IsAsciiAlphanumeric(c)) {
        Reconsume(State::kNamedCharacterReference);
      } else if (c == '#') {
        temporary_buffer_.push_back('#');
        state_ = State::kNumericCharacterReference;
      } else {
        FlushCharacterReference();
        Reconsume(return_state_);
      }
      break;

    case State::kNamedCharacterReference:
      return StepNamedCharacterReference(c);

    case State::kAmbiguousAmpersand:
      if (IsAsciiAlphanumeric(c)) {
        if (IsReturnStateAttributeValue()) {
          AppendCodePoint(current_token_.attributes.back().value, c);
        } else {
          EmitCharacter(c);
        }
      } else {
        if (c == ';') {
          ReportError(Code::kUnknownNamedCharacterReference);
        }
        Reconsume(return_state_);
      }
      break;

    case State::kNumericCharacterReference:
      character_reference_code_ = 0;
      if (c == 'x' || c == 'X') {
        AppendCodePoint(temporary_buffer_, c);
        state_ = State::kHexadecimalCharacterReferenceStart;
      } else if (IsAsciiDigit(c)) {
        Reconsume(State::kDecimalCharacterReference);
      } else {
        ReportError(Code::kAbsenceOfDigitsInNumericCharacterReference);
        FlushCharacterReference();
        Reconsume(return_state_);
      }
      break;

    case State::kHexadecimalCharacterReferenceStart:
      if (IsAsciiHexDigit(c)) {
        Reconsume(State::kHexadecimalCharacterReference);
      } else {
        ReportError(Code::kAbsenceOfDigitsInNumericCharacterReference);
        FlushCharacterReference();
        Reconsume(return_state_);
      }
      break;

    case State::kHexadecimalCharacterReference:
    case State::kDecimalCharacterReference: {
      const bool hexadecimal = state_ == State::kHexadecimalCharacterReference;
      if (hexadecimal ? IsAsciiHexDigit(c) : IsAsciiDigit(c)) {
        // Only whether it is past the end of Unicode matters beyond it.
        character_reference_code_ = std::min<uint32_t>(
            character_reference_code_ * (hexadecimal ? 16 : 10) +
                HexDigitValue(c),
            kMaxCodePoint + 1);
      } else if (c == ';') {
        state_ = State::kNumericCharacterReferenceEnd;
      } else {
        ReportError(Code::kMissingSemicolonAfterCharacterReference);
        Reconsume(State::kNumericCharacterReferenceEnd);
      }
      break;
    }

    case State::kNumericCharacterReferenceEnd:
      StepNumericCharacterReferenceEnd(c);
      break;

    case State::kProcessingInstructionOpen:
      if (c == '_' || IsAsciiAlpha(c)) {
        Reconsume(State::kProcessingInstructionTarget);
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInProcessingInstruction);
        EmitEndOfFile();
      } else {
        ReportError(Code::kInvalidFirstCharacterOfProcessingInstructionTarget);
        ConvertTemporaryBufferToComment();
        Reconsume(State::kBogusComment);
      }
      break;

    case State::kProcessingInstructionTarget:
      if (IsWhitespace(c) || c == '?' || c == '>') {
        if (EqualsIgnoringAsciiCase(temporary_buffer_, u"xml") ||
            EqualsIgnoringAsciiCase(temporary_buffer_, u"xml-stylesheet")) {
          ReportError(Code::kDisallowedProcessingInstructionTarget);
          ConvertTemporaryBufferToComment();
          Reconsume(State::kBogusComment);
        } else {
          current_token_ = HtmlToken();
          current_token_.type = HtmlToken::Type::kProcessingInstruction;
          current_token_.name = temporary_buffer_;
          Reconsume(State::kAfterProcessingInstructionTarget);
        }
      } else if (c == '-' || c == '_' || IsAsciiAlphanumeric(c)) {
        AppendCodePoint(temporary_buffer_, c);
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInProcessingInstruction);
        EmitEndOfFile();
      } else {
        ReportError(Code::kInvalidProcessingInstructionTarget);
        ConvertTemporaryBufferToComment();
        Reconsume(State::kBogusComment);
      }
      break;

    case State::kAfterProcessingInstructionTarget:
      if (!IsWhitespace(c)) {
        Reconsume(State::kProcessingInstructionData);
      }
      break;

    case State::kProcessingInstructionData:
      if (c == '?') {
        state_ = State::kProcessingInstructionQuestionable;
      } else if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInProcessingInstruction);
        EmitEndOfFile();
      } else {
        // U+FFFD for U+0000, as above, which the standard appends as it is.
        AppendCodePoint(current_token_.data, c);
      }
      break;

    case State::kProcessingInstructionQuestionable:
      if (c == '>') {
        state_ = State::kData;
        EmitCurrentToken();
      } else if (c == kEndOfFile) {
        ReportError(Code::kEofInProcessingInstruction);
        EmitEndOfFile();
      } else {
        current_token_.data.push_back('?');
        Reconsume(State::kProcessingInstructionData);
      }
      break;
  }
  return true;
}

void HtmlTokenizer::StepEndTagName(char32_t c, State text_state) {
  if ((IsWhitespace(c) || c == '/' || c == '>') && IsAppropriateEndTag()) {
    if (c == '>') {
      state_ = State::kData;
      EmitCurrentToken();
    } else {
      state_ =
          c == '/' ? State::kSelfClosingStartTag : State::kBeforeAttributeName;
    }
    return;
  }
  if (IsAsciiAlpha(c)) {
    AppendCodePoint(current_token_.name, ToAsciiLower(c));
    AppendCodePoint(temporary_buffer_, c);
    return;
  }
  EmitCharacters(u"</");
  EmitCharacters(temporary_buffer_);
  Reconsume(text_state);
}

bool HtmlTokenizer::StepNamedCharacterReference(char32_t c) {
  // The standard consumes the longest name in the table that the input
  // starts with. Looking ahead first means nothing has to go back, and that
  // it waits for more input while a longer name could still come.
  EntitySearch search;
  const HtmlEntity* match = nullptr;
  size_t match_length = 0;
  size_t length = 0;
  char16_t next = static_cast<char16_t>(c);
  while (search.Advance(next)) {
    ++length;
    if (search.match()) {
      match = search.match();
      match_length = length;
    }
    if (!search.CanExtend()) {
      break;
    }
    const Peek peek = PeekUnit(length - 1, next);
    if (peek == Peek::kNeedMoreInput) {
      Reconsume(State::kNamedCharacterReference);
      return false;
    }
    if (peek == Peek::kEndOfFile) {
      break;
    }
  }
  if (!match) {
    FlushCharacterReference();
    Reconsume(State::kAmbiguousAmpersand);
    return true;
  }

  // For historical reasons a reference in an attribute value that has no
  // semicolon and is followed by = or an alphanumeric stays as it is.
  const bool has_semicolon = match->name.back() == ';';
  bool stays = false;
  if (!has_semicolon && IsReturnStateAttributeValue()) {
    char16_t after;
    const Peek peek = PeekUnit(match_length - 1, after);
    if (peek == Peek::kNeedMoreInput) {
      Reconsume(State::kNamedCharacterReference);
      return false;
    }
    stays = peek == Peek::kCharacter &&
            (after == '=' || IsAsciiAlphanumeric(after));
  }

  AppendCodePoint(temporary_buffer_, c);
  for (size_t i = 1; i < match_length; ++i) {
    ConsumeAscii(1);
    AppendCodePoint(temporary_buffer_, c_);
  }
  if (!stays) {
    if (!has_semicolon) {
      ReportErrorAt(Code::kMissingSemicolonAfterCharacterReference,
                    NextPosition());
    }
    temporary_buffer_.clear();
    AppendCodePoint(temporary_buffer_, match->first);
    if (match->second) {
      AppendCodePoint(temporary_buffer_, match->second);
    }
  }
  FlushCharacterReference();
  state_ = return_state_;
  return true;
}

void HtmlTokenizer::StepNumericCharacterReferenceEnd(char32_t c) {
  char32_t code = character_reference_code_;
  if (code == 0) {
    ReportError(Code::kNullCharacterReference);
    code = kReplacementCharacter;
  } else if (code > kMaxCodePoint) {
    ReportError(Code::kCharacterReferenceOutsideUnicodeRange);
    code = kReplacementCharacter;
  } else if (IsSurrogate(code)) {
    ReportError(Code::kSurrogateCharacterReference);
    code = kReplacementCharacter;
  } else if (IsNoncharacter(code)) {
    ReportError(Code::kNoncharacterCharacterReference);
  } else if (code == '\r' || (IsControl(code) && !IsAsciiWhitespace(code))) {
    ReportError(Code::kControlCharacterReference);
    if (code >= 0x80 && code <= 0x9F && kC1Replacements[code - 0x80]) {
      code = kC1Replacements[code - 0x80];
    }
  }
  temporary_buffer_.clear();
  AppendCodePoint(temporary_buffer_, code);
  FlushCharacterReference();
  // The standard switches to the return state without consuming. Here the
  // current input character is the one after the reference, so it goes back.
  Reconsume(return_state_);
}

}  // namespace mitto
