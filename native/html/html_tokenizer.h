// The tokenization stage of the HTML Standard's parser, which turns a
// document's characters into tokens for the tree builder:
// https://html.spec.whatwg.org/multipage/parsing.html#tokenization
// It has every state of that section and reports every parse error the
// section names. Where Blink's HTMLTokenizer differs from the standard in
// what it emits, it follows Blink, and the place says so.
//
// Upstream: third_party/blink/renderer/core/html/parser/html_tokenizer.h
// Upstream: third_party/blink/renderer/core/html/parser/html_tokenizer.cc
// Upstream: third_party/blink/renderer/core/html/parser/input_stream_preprocessor.h
// Upstream: third_party/blink/renderer/core/html/parser/html_entity_parser.cc
// Upstream: third_party/blink/renderer/core/html/parser/html_entity_search.cc

#ifndef MITTO_HTML_HTML_TOKENIZER_H_
#define MITTO_HTML_HTML_TOKENIZER_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/html/html_parse_error.h"
#include "mitto/html/html_token.h"

namespace mitto {

class HtmlTokenizer {
 public:
  // The states of the standard's tokenization section, in its order.
  enum class State {
    kData,
    kRcdata,
    kRawtext,
    kScriptData,
    kPlaintext,
    kTagOpen,
    kEndTagOpen,
    kTagName,
    kRcdataLessThanSign,
    kRcdataEndTagOpen,
    kRcdataEndTagName,
    kRawtextLessThanSign,
    kRawtextEndTagOpen,
    kRawtextEndTagName,
    kScriptDataLessThanSign,
    kScriptDataEndTagOpen,
    kScriptDataEndTagName,
    kScriptDataEscapeStart,
    kScriptDataEscapeStartDash,
    kScriptDataEscaped,
    kScriptDataEscapedDash,
    kScriptDataEscapedDashDash,
    kScriptDataEscapedLessThanSign,
    kScriptDataEscapedEndTagOpen,
    kScriptDataEscapedEndTagName,
    kScriptDataDoubleEscapeStart,
    kScriptDataDoubleEscaped,
    kScriptDataDoubleEscapedDash,
    kScriptDataDoubleEscapedDashDash,
    kScriptDataDoubleEscapedLessThanSign,
    kScriptDataDoubleEscapeEnd,
    kBeforeAttributeName,
    kAttributeName,
    kAfterAttributeName,
    kBeforeAttributeValue,
    kAttributeValueDoubleQuoted,
    kAttributeValueSingleQuoted,
    kAttributeValueUnquoted,
    kAfterAttributeValueQuoted,
    kSelfClosingStartTag,
    kBogusComment,
    kMarkupDeclarationOpen,
    kCommentStart,
    kCommentStartDash,
    kComment,
    kCommentLessThanSign,
    kCommentLessThanSignBang,
    kCommentLessThanSignBangDash,
    kCommentLessThanSignBangDashDash,
    kCommentEndDash,
    kCommentEnd,
    kCommentEndBang,
    kDoctype,
    kBeforeDoctypeName,
    kDoctypeName,
    kAfterDoctypeName,
    kAfterDoctypePublicKeyword,
    kBeforeDoctypePublicIdentifier,
    kDoctypePublicIdentifierDoubleQuoted,
    kDoctypePublicIdentifierSingleQuoted,
    kAfterDoctypePublicIdentifier,
    kBetweenDoctypePublicAndSystemIdentifiers,
    kAfterDoctypeSystemKeyword,
    kBeforeDoctypeSystemIdentifier,
    kDoctypeSystemIdentifierDoubleQuoted,
    kDoctypeSystemIdentifierSingleQuoted,
    kAfterDoctypeSystemIdentifier,
    kBogusDoctype,
    kCdataSection,
    kCdataSectionBracket,
    kCdataSectionEnd,
    kProcessingInstructionOpen,
    kProcessingInstructionTarget,
    kAfterProcessingInstructionTarget,
    kProcessingInstructionData,
    kProcessingInstructionQuestionable,
    kCharacterReference,
    kNamedCharacterReference,
    kAmbiguousAmpersand,
    kNumericCharacterReference,
    kHexadecimalCharacterReferenceStart,
    kHexadecimalCharacterReference,
    kDecimalCharacterReference,
    kNumericCharacterReferenceEnd,
  };

  struct Options {
    // Processing instructions, <?target data>, which Blink parses with its
    // feature HTMLProcessingInstruction, on for stable in the Engine Release.
    // Off, <? starts a bogus comment, as it did before the standard had them.
    bool processing_instructions = true;
    // Whether to keep the parse errors for TakeParseErrors().
    bool collect_parse_errors = false;
  };

  // Where a parse error is: the line and the column of the character it is
  // at, from 1, in UTF-16 code units as Blink counts them, after the input's
  // CR LF and CR became LF. One at the end of the input is just past its last
  // character.
  struct ParseError {
    friend bool operator==(const ParseError&, const ParseError&) = default;

    HtmlParseErrorCode code;
    int line;
    int column;
  };

  HtmlTokenizer();
  explicit HtmlTokenizer(const Options& options);
  HtmlTokenizer(const HtmlTokenizer&) = delete;
  HtmlTokenizer& operator=(const HtmlTokenizer&) = delete;
  ~HtmlTokenizer();

  // Adds to the end of the input stream, in pieces of any size, as a network
  // body arrives. A piece may end inside a CR LF or a surrogate pair.
  void Append(std::u16string_view input);

  // Ends the input stream.
  void Finish();
  // Whether Finish() has been called.
  bool HasSeenEndOfFile() const { return finished_; }

  // The HTML Standard's insertion point, as Blink's HTMLInputStream and
  // InsertionPointRecord keep it for document.write(): the parser sets one
  // just before the next input character while a script it reached runs, and
  // takes it back to the one before once the script is done. While one is
  // set, the tokenizer reads no further than it and does not see the end of
  // the input, and Insert() adds there.
  void PushInsertionPoint();
  void PopInsertionPoint();
  bool HasInsertionPoint() const { return !insertion_points_.empty(); }
  // Adds what a script wrote at the insertion point, or at the end of the
  // input while none is set, as for a document that document.open() opened.
  // Its CR LF and CR become LF as the network's do, and its characters do
  // not count in the positions of what comes after them, as Blink excludes
  // them from line numbers.
  void Insert(std::u16string_view input);

  // Moves the next token into `token` and returns true, or returns false if
  // the input so far holds no more. Characters come as character tokens,
  // and a run of them may come in several. After the end-of-file token it
  // always returns false.
  bool NextToken(HtmlToken& token);

  // The tree builder switches the state right after it gets a start tag, to
  // one of data, RCDATA, RAWTEXT, script data or PLAINTEXT. A test can also
  // start in the CDATA section state.
  State state() const { return state_; }
  void SetState(State state);

  // The tag name that makes an end tag appropriate, which the tokenizer sets
  // itself whenever it emits a start tag.
  void SetLastStartTagName(std::u16string_view name);

  // Blink's tree builder turns this on in its text insertion mode and in
  // foreign content. Off, Blink drops U+0000 in the data, RCDATA and RAWTEXT
  // states, where the standard emits it for the tree builder to ignore.
  void SetForceNullCharacterReplacement(bool value) {
    force_null_character_replacement_ = value;
  }

  // Whether there is an adjusted current node that is not an element in the
  // HTML namespace, which lets <![CDATA[ open a CDATA section.
  void SetAllowCdata(bool value) { allow_cdata_ = value; }

  // Where the next input character is, from zero, in UTF-16 code units, as
  // Blink's HTMLDocumentParser::GetTextPosition() gives it: right after the
  // token that NextToken() gave last, such as a script's start tag.
  struct ZeroBasedPosition {
    int line;
    int column;
  };
  ZeroBasedPosition TextPosition() const;

  // The parse errors since the last call, in the order they happened, if
  // Options::collect_parse_errors is on.
  std::vector<ParseError> TakeParseErrors();

 private:
  enum class Peek {
    kCharacter,
    kEndOfFile,
    kNeedMoreInput,
  };

  enum class LookAheadResult {
    kMatch,
    kNoMatch,
    kNeedMoreInput,
  };

  struct Position {
    int line;
    int column;
  };

  // Runs one state for one code point. Returns false if the input ran out
  // first.
  bool Step();

  // Moves to the next code point of the input into `c_`: the end-of-file
  // marker once Finish() has been called and the input is used up. Returns
  // false if the input has run out before it.
  bool Consume();
  // The code unit `offset` units past the current input character.
  Peek PeekUnit(size_t offset, char16_t& unit) const;
  // Whether the input from the current input character on starts with
  // `expected`, which is ASCII and lower case if `ignore_case`.
  LookAheadResult LookAhead(std::u16string_view expected,
                            bool ignore_case) const;
  // Consumes `count` more code points, all of them ASCII.
  void ConsumeAscii(size_t count);

  Position CurrentPosition() const;
  // The position of the next input character.
  Position NextPosition() const;
  void ReportError(HtmlParseErrorCode code);
  void ReportErrorAt(HtmlParseErrorCode code, const Position& position);

  void Reconsume(State state) {
    state_ = state;
    reconsume_ = true;
  }

  void EmitCharacter(char32_t c);
  void EmitCharacters(std::u16string_view characters);
  void EmitCurrentToken();
  void EmitEndOfFile();
  void FlushCharacters();

  void CreateStartTag();
  void CreateEndTag();
  void CreateComment(std::u16string_view data);
  void CreateDoctype();
  void StartAttribute();
  void FinishAttributeName();
  void DropAttributeIfDuplicate();
  bool IsAppropriateEndTag() const;

  bool IsReturnStateAttributeValue() const;
  void FlushCharacterReference();

  // The end tag name states of RCDATA, RAWTEXT and script data, which differ
  // only in the state they go back to.
  void StepEndTagName(char32_t c, State text_state);
  // Returns false if it needs more input to decide.
  bool StepNamedCharacterReference(char32_t c);
  // The standard's "convert the temporary buffer to a comment".
  void ConvertTemporaryBufferToComment();
  void StepNumericCharacterReferenceEnd(char32_t c);

  const Options options_;

  State state_ = State::kData;
  State return_state_ = State::kData;
  bool force_null_character_replacement_ = false;
  bool allow_cdata_ = false;

  // Where the input that the tokenizer may read so far ends: the innermost
  // insertion point, or the end of the input.
  size_t ReadableEnd() const {
    return insertion_points_.empty() ? input_.size() : insertion_points_.back();
  }
  // Whether the tokenizer has reached the end of the input once it has read
  // what it may: Finish() has been called and no insertion point is set.
  bool ReadsEndOfFile() const { return finished_ && insertion_points_.empty(); }

  // The input, with CR LF and CR already LF, and what of it is used up.
  std::u16string input_;
  size_t input_position_ = 0;
  bool last_input_was_cr_ = false;
  bool finished_ = false;
  // The insertion points, innermost last, as offsets in `input_`. An outer
  // one is never before an inner one.
  std::vector<size_t> insertion_points_;
  // Where what scripts wrote ends in `input_`: what is unread before it came
  // from Insert(), and what is after it from the network, as a script writes
  // only before what the network has given.
  size_t inserted_end_ = 0;
  bool last_inserted_was_cr_ = false;

  // The current input character, and whether it was U+0000 before it became
  // U+FFFD, as Blink replaces it in most states before they see it.
  char32_t c_ = 0;
  bool c_was_null_ = false;
  bool reconsume_ = false;

  // Where the current input character is, and how many code units it has.
  int line_ = 1;
  int column_ = 0;
  int current_length_ = 1;
  bool current_is_line_feed_ = false;

  HtmlToken current_token_;
  bool current_attribute_is_duplicate_ = false;
  // The current tag's attribute names, once it has too many to compare one
  // by one.
  std::set<std::u16string> attribute_names_;
  std::u16string last_start_tag_name_;
  std::u16string temporary_buffer_;
  uint32_t character_reference_code_ = 0;

  // Characters emitted and not yet in a token, and the tokens emitted and
  // not yet taken.
  std::u16string characters_;
  std::deque<HtmlToken> tokens_;
  bool emitted_end_of_file_ = false;

  std::vector<ParseError> errors_;
};

}  // namespace mitto

#endif  // MITTO_HTML_HTML_TOKENIZER_H_
