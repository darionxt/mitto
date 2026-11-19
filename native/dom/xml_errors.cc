#include "mitto/dom/xml_errors.h"

#include <optional>
#include <string>

#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/text.h"

namespace mitto {

namespace {

constexpr int kMaxErrors = 25;

Element* CreateHtmlElement(Document& document, const std::u16string& name) {
  return document.CreateElement(html_names::kXhtmlNamespace, std::nullopt,
                                name);
}

// CreateXHTMLParserErrorHeader().
Element* CreateParserErrorHeader(Document& document,
                                 const std::u16string& error_messages) {
  Element* const report = CreateHtmlElement(document, u"parsererror");
  report->SetAttributeWithoutValidation(
      u"style",
      u"display: block; white-space: pre; border: 2px solid #c77; padding: 0 "
      u"1em 0 1em; margin: 1em; background-color: #fdd; color: black");
  Element* h3 = CreateHtmlElement(document, u"h3");
  report->ParserAppendChild(h3);
  h3->ParserAppendChild(
      document.CreateTextNode(u"This page contains the following errors:"));
  Element* const fixed = CreateHtmlElement(document, u"div");
  fixed->SetAttributeWithoutValidation(u"style",
                                       u"font-family:monospace;font-size:12px");
  report->ParserAppendChild(fixed);
  fixed->ParserAppendChild(document.CreateTextNode(error_messages));
  h3 = CreateHtmlElement(document, u"h3");
  report->ParserAppendChild(h3);
  h3->ParserAppendChild(document.CreateTextNode(
      u"Below is a rendering of the page up to the first error."));
  return report;
}

}  // namespace

void XmlErrors::HandleError(Type type,
                            std::string_view message,
                            MarkupPosition where) {
  if (type != Type::kFatal &&
      (error_count_ >= kMaxErrors || last_error_position_.line == where.line ||
       last_error_position_.column == where.column)) {
    return;
  }
  // <type> on line <line> at column <column>: <message>
  error_messages_ += type == Type::kWarning ? u"warning" : u"error";
  error_messages_ += u" on line ";
  error_messages_ += base::NumberToString16(where.line + 1);
  error_messages_ += u" at column ";
  error_messages_ += base::NumberToString16(where.column + 1);
  error_messages_ += u": ";
  error_messages_ += base::UTF8ToUTF16(message);
  last_error_position_ = where;
  ++error_count_;
}

void XmlErrors::InsertErrorMessageBlock(Document& document) const {
  Element* document_element = document.DocumentElement();
  if (!document_element) {
    Element* const root = CreateHtmlElement(document, u"html");
    Element* const body = CreateHtmlElement(document, u"body");
    root->ParserAppendChild(body);
    document.ParserAppendChild(root);
    document_element = body;
  } else if (document_element->NamespaceURI() == html_names::kSvgNamespace) {
    Element* const root = CreateHtmlElement(document, u"html");
    Element* const head = CreateHtmlElement(document, u"head");
    Element* const style = CreateHtmlElement(document, u"style");
    head->ParserAppendChild(style);
    style->ParserAppendChild(document.CreateTextNode(
        u"html, body { height: 100% } parsererror + svg { width: 100%; "
        u"height: 100% }"));
    root->ParserAppendChild(head);
    Element* const body = CreateHtmlElement(document, u"body");
    root->ParserAppendChild(body);
    document.ParserRemoveChild(*document_element);
    body->ParserAppendChild(document_element);
    document.ParserAppendChild(root);
    document_element = body;
  }
  // A document that an XSL transformation made adds a paragraph that says so,
  // which comes with XSLT.
  Element* const report = CreateParserErrorHeader(document, error_messages_);
  if (Node* const first_child = document_element->FirstChild()) {
    document_element->ParserInsertBefore(report, *first_child);
  } else {
    document_element->ParserAppendChild(report);
  }
}

bool MatchesXhtmlSubsetDtd(std::u16string_view public_id) {
  constexpr std::u16string_view kPublicIds[] = {
      u"-//W3C//DTD XHTML 1.0 Transitional//EN",
      u"-//W3C//DTD XHTML 1.1//EN",
      u"-//W3C//DTD XHTML 1.0 Strict//EN",
      u"-//W3C//DTD XHTML 1.0 Frameset//EN",
      u"-//W3C//DTD XHTML Basic 1.0//EN",
      u"-//W3C//DTD XHTML 1.1 plus MathML 2.0//EN",
      u"-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN",
      u"-//W3C//DTD MathML 2.0//EN",
      u"-//WAPFORUM//DTD XHTML Mobile 1.0//EN",
      u"-//WAPFORUM//DTD XHTML Mobile 1.1//EN",
      u"-//WAPFORUM//DTD XHTML Mobile 1.2//EN",
  };
  for (const std::u16string_view id : kPublicIds) {
    if (public_id == id) {
      return true;
    }
  }
  return false;
}

}  // namespace mitto
