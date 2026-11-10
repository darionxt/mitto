#include "mitto/html/html_parse_error.h"

#include "base/notreached.h"

namespace mitto {

std::string_view HtmlParseErrorName(HtmlParseErrorCode code) {
  switch (code) {
    case HtmlParseErrorCode::kAbruptClosingOfEmptyComment:
      return "abrupt-closing-of-empty-comment";
    case HtmlParseErrorCode::kAbruptDoctypePublicIdentifier:
      return "abrupt-doctype-public-identifier";
    case HtmlParseErrorCode::kAbruptDoctypeSystemIdentifier:
      return "abrupt-doctype-system-identifier";
    case HtmlParseErrorCode::kAbsenceOfDigitsInNumericCharacterReference:
      return "absence-of-digits-in-numeric-character-reference";
    case HtmlParseErrorCode::kCdataInHtmlContent:
      return "cdata-in-html-content";
    case HtmlParseErrorCode::kCharacterReferenceOutsideUnicodeRange:
      return "character-reference-outside-unicode-range";
    case HtmlParseErrorCode::kControlCharacterInInputStream:
      return "control-character-in-input-stream";
    case HtmlParseErrorCode::kControlCharacterReference:
      return "control-character-reference";
    case HtmlParseErrorCode::kDisallowedProcessingInstructionTarget:
      return "disallowed-processing-instruction-target";
    case HtmlParseErrorCode::kDuplicateAttribute:
      return "duplicate-attribute";
    case HtmlParseErrorCode::kEndTagWithAttributes:
      return "end-tag-with-attributes";
    case HtmlParseErrorCode::kEndTagWithTrailingSolidus:
      return "end-tag-with-trailing-solidus";
    case HtmlParseErrorCode::kEofBeforeTagName:
      return "eof-before-tag-name";
    case HtmlParseErrorCode::kEofInCdata:
      return "eof-in-cdata";
    case HtmlParseErrorCode::kEofInComment:
      return "eof-in-comment";
    case HtmlParseErrorCode::kEofInDoctype:
      return "eof-in-doctype";
    case HtmlParseErrorCode::kEofInProcessingInstruction:
      return "eof-in-processing-instruction";
    case HtmlParseErrorCode::kEofInScriptHtmlCommentLikeText:
      return "eof-in-script-html-comment-like-text";
    case HtmlParseErrorCode::kEofInTag:
      return "eof-in-tag";
    case HtmlParseErrorCode::kIncorrectlyClosedComment:
      return "incorrectly-closed-comment";
    case HtmlParseErrorCode::kIncorrectlyOpenedComment:
      return "incorrectly-opened-comment";
    case HtmlParseErrorCode::kInvalidCharacterSequenceAfterDoctypeName:
      return "invalid-character-sequence-after-doctype-name";
    case HtmlParseErrorCode::
        kInvalidFirstCharacterOfProcessingInstructionTarget:
      return "invalid-first-character-of-processing-instruction-target";
    case HtmlParseErrorCode::kInvalidFirstCharacterOfTagName:
      return "invalid-first-character-of-tag-name";
    case HtmlParseErrorCode::kInvalidProcessingInstructionTarget:
      return "invalid-processing-instruction-target";
    case HtmlParseErrorCode::kMissingAttributeValue:
      return "missing-attribute-value";
    case HtmlParseErrorCode::kMissingDoctypeName:
      return "missing-doctype-name";
    case HtmlParseErrorCode::kMissingDoctypePublicIdentifier:
      return "missing-doctype-public-identifier";
    case HtmlParseErrorCode::kMissingDoctypeSystemIdentifier:
      return "missing-doctype-system-identifier";
    case HtmlParseErrorCode::kMissingEndTagName:
      return "missing-end-tag-name";
    case HtmlParseErrorCode::kMissingQuoteBeforeDoctypePublicIdentifier:
      return "missing-quote-before-doctype-public-identifier";
    case HtmlParseErrorCode::kMissingQuoteBeforeDoctypeSystemIdentifier:
      return "missing-quote-before-doctype-system-identifier";
    case HtmlParseErrorCode::kMissingSemicolonAfterCharacterReference:
      return "missing-semicolon-after-character-reference";
    case HtmlParseErrorCode::kMissingWhitespaceAfterDoctypePublicKeyword:
      return "missing-whitespace-after-doctype-public-keyword";
    case HtmlParseErrorCode::kMissingWhitespaceAfterDoctypeSystemKeyword:
      return "missing-whitespace-after-doctype-system-keyword";
    case HtmlParseErrorCode::kMissingWhitespaceBeforeDoctypeName:
      return "missing-whitespace-before-doctype-name";
    case HtmlParseErrorCode::kMissingWhitespaceBetweenAttributes:
      return "missing-whitespace-between-attributes";
    case HtmlParseErrorCode::
        kMissingWhitespaceBetweenDoctypePublicAndSystemIdentifiers:
      return "missing-whitespace-between-doctype-public-and-system-"
             "identifiers";
    case HtmlParseErrorCode::kNestedComment:
      return "nested-comment";
    case HtmlParseErrorCode::kNoncharacterCharacterReference:
      return "noncharacter-character-reference";
    case HtmlParseErrorCode::kNoncharacterInInputStream:
      return "noncharacter-in-input-stream";
    case HtmlParseErrorCode::kNullCharacterReference:
      return "null-character-reference";
    case HtmlParseErrorCode::kSurrogateCharacterReference:
      return "surrogate-character-reference";
    case HtmlParseErrorCode::kSurrogateInInputStream:
      return "surrogate-in-input-stream";
    case HtmlParseErrorCode::kUnexpectedCharacterAfterDoctypeSystemIdentifier:
      return "unexpected-character-after-doctype-system-identifier";
    case HtmlParseErrorCode::kUnexpectedCharacterInAttributeName:
      return "unexpected-character-in-attribute-name";
    case HtmlParseErrorCode::kUnexpectedCharacterInUnquotedAttributeValue:
      return "unexpected-character-in-unquoted-attribute-value";
    case HtmlParseErrorCode::kUnexpectedEqualsSignBeforeAttributeName:
      return "unexpected-equals-sign-before-attribute-name";
    case HtmlParseErrorCode::kUnexpectedNullCharacter:
      return "unexpected-null-character";
    case HtmlParseErrorCode::kUnexpectedQuestionMarkInsteadOfTagName:
      return "unexpected-question-mark-instead-of-tag-name";
    case HtmlParseErrorCode::kUnexpectedSolidusInTag:
      return "unexpected-solidus-in-tag";
    case HtmlParseErrorCode::kUnknownNamedCharacterReference:
      return "unknown-named-character-reference";
  }
  NOTREACHED();
}

}  // namespace mitto
