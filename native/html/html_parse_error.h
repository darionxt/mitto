// The parse errors that the HTML Standard names for its tokenizer:
// https://html.spec.whatwg.org/multipage/parsing.html#parse-errors
// Blink reports none of them, so each comes from the standard alone.

#ifndef MITTO_HTML_HTML_PARSE_ERROR_H_
#define MITTO_HTML_HTML_PARSE_ERROR_H_

#include <string_view>

namespace mitto {

enum class HtmlParseErrorCode {
  kAbruptClosingOfEmptyComment,
  kAbruptDoctypePublicIdentifier,
  kAbruptDoctypeSystemIdentifier,
  kAbsenceOfDigitsInNumericCharacterReference,
  kCdataInHtmlContent,
  kCharacterReferenceOutsideUnicodeRange,
  kControlCharacterInInputStream,
  kControlCharacterReference,
  kDisallowedProcessingInstructionTarget,
  kDuplicateAttribute,
  kEndTagWithAttributes,
  kEndTagWithTrailingSolidus,
  kEofBeforeTagName,
  kEofInCdata,
  kEofInComment,
  kEofInDoctype,
  kEofInProcessingInstruction,
  kEofInScriptHtmlCommentLikeText,
  kEofInTag,
  kIncorrectlyClosedComment,
  kIncorrectlyOpenedComment,
  kInvalidCharacterSequenceAfterDoctypeName,
  kInvalidFirstCharacterOfProcessingInstructionTarget,
  kInvalidFirstCharacterOfTagName,
  kInvalidProcessingInstructionTarget,
  kMissingAttributeValue,
  kMissingDoctypeName,
  kMissingDoctypePublicIdentifier,
  kMissingDoctypeSystemIdentifier,
  kMissingEndTagName,
  kMissingQuoteBeforeDoctypePublicIdentifier,
  kMissingQuoteBeforeDoctypeSystemIdentifier,
  kMissingSemicolonAfterCharacterReference,
  kMissingWhitespaceAfterDoctypePublicKeyword,
  kMissingWhitespaceAfterDoctypeSystemKeyword,
  kMissingWhitespaceBeforeDoctypeName,
  kMissingWhitespaceBetweenAttributes,
  kMissingWhitespaceBetweenDoctypePublicAndSystemIdentifiers,
  kNestedComment,
  kNoncharacterCharacterReference,
  kNoncharacterInInputStream,
  kNullCharacterReference,
  kSurrogateCharacterReference,
  kSurrogateInInputStream,
  kUnexpectedCharacterAfterDoctypeSystemIdentifier,
  kUnexpectedCharacterInAttributeName,
  kUnexpectedCharacterInUnquotedAttributeValue,
  kUnexpectedEqualsSignBeforeAttributeName,
  kUnexpectedNullCharacter,
  // The standard named this before it had processing instructions. The
  // tokenizer reports it with them off.
  kUnexpectedQuestionMarkInsteadOfTagName,
  kUnexpectedSolidusInTag,
  kUnknownNamedCharacterReference,
};

// The error's code as the standard writes it, such as "eof-in-tag".
std::string_view HtmlParseErrorName(HtmlParseErrorCode code);

}  // namespace mitto

#endif  // MITTO_HTML_HTML_PARSE_ERROR_H_
