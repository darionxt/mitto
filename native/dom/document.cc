#include "mitto/dom/document.h"

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_document.h"
#include "mitto/dom/comment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_document.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_element_factory.h"
#include "mitto/dom/html_head_element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/text.h"
#include "mitto/dom/window.h"

namespace mitto {

namespace {

// Blink's IsValidElementName(), the DOM standard's valid element local name.
bool IsValidElementName(std::u16string_view name) {
  if (name.empty()) {
    return false;
  }
  const auto is_ascii_alpha = [](char16_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  };
  if (is_ascii_alpha(name[0])) {
    for (const char16_t c : name.substr(1)) {
      if (c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\f' ||
          c == '\r' || c == '/' || c == '>') {
        return false;
      }
    }
    return true;
  }
  if (name[0] < 0x80 && name[0] != ':' && name[0] != '_') {
    return false;
  }
  for (const char16_t c : name.substr(1)) {
    if (!is_ascii_alpha(c) && !(c >= '0' && c <= '9') && c != '-' && c != '.' &&
        c != ':' && c != '_' && c < 0x80) {
      return false;
    }
  }
  return true;
}

void ThrowInvalidTagName(const std::u16string& name,
                         bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException(
      "InvalidCharacterError",
      base::StrCat({"The tag name provided ('", base::UTF16ToUTF8(name),
                    "') is not a valid name."}));
}

bool IsAsciiWhitespace(char16_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// The HTML Standard's "strip and collapse ASCII whitespace".
std::u16string StripAndCollapseWhitespace(const std::u16string& text) {
  std::u16string result;
  bool in_whitespace = false;
  for (const char16_t c : text) {
    if (IsAsciiWhitespace(c)) {
      in_whitespace = true;
      continue;
    }
    if (in_whitespace && !result.empty()) {
      result += u' ';
    }
    in_whitespace = false;
    result += c;
  }
  return result;
}

}  // namespace

// static
Document* Document::Create() {
  return bindings::MakeGarbageCollected<Document>(false);
}

// static
Document* Document::CreateHTML() {
  return HTMLDocument::Create();
}

Document::Document(bool is_html) : Node(nullptr), is_html_(is_html) {
  SetDocumentToSelf(this);
}

Document::~Document() = default;

const bindings::WrapperTypeInfo* Document::GetWrapperTypeInfo() const {
  return &bindings::V8Document::kWrapperTypeInfo;
}

void Document::Trace(cppgc::Visitor* visitor) const {
  Node::Trace(visitor);
  visitor->Trace(window_);
}

std::u16string Document::CompatMode() const {
  return InQuirksMode() ? u"BackCompat" : u"CSS1Compat";
}

std::u16string Document::ContentType() const {
  return is_html_ ? u"text/html" : u"application/xml";
}

DocumentType* Document::Doctype() const {
  for (Node* child = FirstChild(); child; child = child->NextSibling()) {
    if (child->IsDocumentTypeNode()) {
      return static_cast<DocumentType*>(child);
    }
  }
  return nullptr;
}

std::u16string Document::Title() const {
  // The first title element in the document, in tree order.
  for (const Node* node = FirstChild(); node; node = node->NextInTree(this)) {
    if (node->IsElementNode() &&
        static_cast<const Element*>(node)->HasHTMLTagName(u"title")) {
      return StripAndCollapseWhitespace(node->ChildTextContent());
    }
  }
  return std::u16string();
}

void Document::SetTitle(const std::u16string& title) {
  Element* const root = DocumentElement();
  if (!root || !root->IsHTMLElement()) {
    return;
  }
  Element* title_element = nullptr;
  for (Node* node = FirstChild(); node; node = node->NextInTree(this)) {
    if (node->IsElementNode() &&
        static_cast<Element*>(node)->HasHTMLTagName(u"title")) {
      title_element = static_cast<Element*>(node);
      break;
    }
  }
  if (!title_element) {
    HTMLHeadElement* const head = Head();
    if (!head) {
      return;
    }
    title_element =
        CreateElement(html_names::kXhtmlNamespace, std::nullopt, u"title");
    head->ParserAppendChild(title_element);
  }
  // HTMLTitleElement's text setter: replace all with one Text node.
  title_element->RemoveChildren();
  if (!title.empty()) {
    title_element->ParserAppendChild(Text::Create(*this, title));
  }
}

HTMLElement* Document::Body() const {
  Element* const root = DocumentElement();
  if (!root || !root->HasHTMLTagName(u"html")) {
    return nullptr;
  }
  for (Node* child = root->FirstChild(); child; child = child->NextSibling()) {
    if (child->IsElementNode() &&
        (static_cast<Element*>(child)->HasHTMLTagName(u"body") ||
         static_cast<Element*>(child)->HasHTMLTagName(u"frameset"))) {
      return static_cast<HTMLElement*>(child);
    }
  }
  return nullptr;
}

HTMLHeadElement* Document::Head() const {
  Element* const root = DocumentElement();
  if (!root || !root->HasHTMLTagName(u"html")) {
    return nullptr;
  }
  for (Node* child = root->FirstChild(); child; child = child->NextSibling()) {
    if (child->IsElementNode() &&
        static_cast<Element*>(child)->HasHTMLTagName(u"head")) {
      return static_cast<HTMLHeadElement*>(child);
    }
  }
  return nullptr;
}

Comment* Document::CreateComment(const std::u16string& data) {
  return Comment::Create(*this, data);
}

Text* Document::CreateTextNode(const std::u16string& data) {
  return Text::Create(*this, data);
}

Element* Document::CreateElementForBinding(
    const std::u16string& local_name,
    bindings::ExceptionState& exception_state) {
  if (!IsValidElementName(local_name)) {
    ThrowInvalidTagName(local_name, exception_state);
    return nullptr;
  }
  if (is_html_) {
    return CreateHTMLElement(*this, base::ToLowerASCII(local_name));
  }
  return Element::Create(*this, std::nullopt, std::nullopt, local_name);
}

Element* Document::CreateElementForBinding(
    const std::u16string& local_name,
    const bindings::DOMStringOrElementCreationOptions& options,
    bindings::ExceptionState& exception_state) {
  // Its `is` names a customized built-in element, which needs a definition
  // that the Runtime, without custom elements, never has.
  return CreateElementForBinding(local_name, exception_state);
}

Element* Document::GetElementById(const std::u16string& id) const {
  if (id.empty()) {
    return nullptr;
  }
  for (Node* node = FirstChild(); node; node = node->NextInTree(this)) {
    if (!node->IsElementNode()) {
      continue;
    }
    const std::u16string* value =
        static_cast<Element*>(node)->FindAttribute(u"id");
    if (value && *value == id) {
      return static_cast<Element*>(node);
    }
  }
  return nullptr;
}

Element* Document::CreateElement(
    const std::optional<std::u16string>& namespace_uri,
    const std::optional<std::u16string>& prefix,
    const std::u16string& local_name) {
  if (namespace_uri == html_names::kXhtmlNamespace && !prefix) {
    return CreateHTMLElement(*this, local_name);
  }
  return Element::Create(*this, namespace_uri, prefix, local_name);
}

Node* Document::CloneWithoutChildren(Document& document) const {
  Document* const copy = is_html_ ? CreateHTML() : Create();
  copy->SetCompatibilityMode(compat_mode_);
  return copy;
}

uint16_t Document::GetNodeType() const {
  return kDocumentNode;
}

std::u16string Document::NodeName() const {
  return u"#document";
}

}  // namespace mitto
