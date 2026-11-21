// The errors of an XML parse and the block that shows them, as Blink's
// XMLErrors has them: each error, warning or fatal error with its line and
// column, up to 25 that are not fatal, and the parsererror element that goes
// first in the document once the parser has stopped on an error.
//
// Upstream: third_party/blink/renderer/core/xml/parser/xml_errors.h
// Upstream: third_party/blink/renderer/core/xml/parser/xml_errors.cc

#ifndef MITTO_DOM_XML_ERRORS_H_
#define MITTO_DOM_XML_ERRORS_H_

#include <string>
#include <string_view>

#include "mitto/dom/markup_position.h"

namespace mitto {

class Document;

class XmlErrors {
 public:
  enum class Type {
    kWarning,
    kNonFatal,
    kFatal,
  };

  XmlErrors() = default;

  // An error at a zero-based position, which the message gives one-based.
  void HandleError(Type type, std::string_view message, MarkupPosition where);

  // InsertErrorMessageBlock(): the parsererror element with the errors, first
  // in the document element, or in a body that an HTML element puts the
  // document's own element in when that is an SVG element or missing.
  void InsertErrorMessageBlock(Document& document) const;

 private:
  int error_count_ = 0;
  // TextPosition::BelowRangePosition(), which no error has.
  MarkupPosition last_error_position_{-2, -2};
  std::u16string error_messages_;
};

// MatchesXHTMLSubsetDTD(): whether a doctype's public identifier names one of
// the XHTML DTDs whose entities the HTML Standard has an XML parser know.
// Upstream: third_party/blink/renderer/core/xml/parser/xhtml_subset.cc
bool MatchesXhtmlSubsetDtd(std::u16string_view public_id);

}  // namespace mitto

#endif  // MITTO_DOM_XML_ERRORS_H_
