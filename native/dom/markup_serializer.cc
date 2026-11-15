#include "mitto/dom/markup_serializer.h"

#include <string_view>

#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_template_element.h"
#include "mitto/dom/node.h"

namespace mitto {

namespace {

// Elements that the serializer writes with no children and no end tag.
bool IsVoidElement(const Element& element) {
  if (!element.IsHTMLElement()) {
    return false;
  }
  constexpr std::u16string_view kVoidElements[] = {
      u"area",  u"base",  u"basefont", u"bgsound", u"br",    u"col",
      u"embed", u"frame", u"hr",       u"img",     u"input", u"keygen",
      u"link",  u"meta",  u"param",    u"source",  u"track", u"wbr"};
  for (const std::u16string_view name : kVoidElements) {
    if (element.LocalName() == name) {
      return true;
    }
  }
  return false;
}

// Whether `text` is written as it is, as Blink's
// MarkupFormatter::EntityMaskForText() has it: in an HTML element of these
// names, with no prefix, and in noscript where the document has a window,
// whose scripts a Page always runs. A document of no window, such as a
// template's contents or one that createHTMLDocument() made, has noscript's
// text escaped.
bool HoldsRawText(const Node& text) {
  const Node* const parent = text.ParentNode();
  if (!parent || !parent->IsElementNode()) {
    return false;
  }
  const auto& element = static_cast<const Element&>(*parent);
  if (!element.IsHTMLElement() || element.Prefix().has_value()) {
    return false;
  }
  constexpr std::u16string_view kRawTextElements[] = {
      u"style",   u"script",   u"xmp",      u"iframe",
      u"noembed", u"noframes", u"plaintext"};
  for (const std::u16string_view name : kRawTextElements) {
    if (element.LocalName() == name) {
      return true;
    }
  }
  return element.LocalName() == u"noscript" &&
         text.GetDocument().DefaultView() != nullptr;
}

// Blink's kEntityMaskInHtmlPcdata and kEntityMaskInHtmlAttributeValue.
void AppendEscaped(std::u16string& markup,
                   std::u16string_view text,
                   bool in_attribute) {
  for (const char16_t c : text) {
    switch (c) {
      case '&':
        markup += u"&amp;";
        break;
      case 0xA0:
        markup += u"&nbsp;";
        break;
      case '<':
        markup += u"&lt;";
        break;
      case '>':
        markup += u"&gt;";
        break;
      case '"':
        markup += in_attribute ? u"&quot;" : u"\"";
        break;
      default:
        markup += c;
    }
  }
}

void AppendNode(std::u16string& markup, const Node& node);

void AppendChildren(std::u16string& markup, const Node& node) {
  for (const Node* child = node.FirstChild(); child;
       child = child->NextSibling()) {
    AppendNode(markup, *child);
  }
}

// The HTML Standard's attribute's serialized name, as Blink's
// ResolveAttributePrefixForHtml() writes it.
std::u16string SerializedAttributeName(const QualifiedName& name) {
  std::optional<std::u16string> prefix = name.prefix;
  if (name.namespace_uri == html_names::kXmlnsNamespace) {
    if (!name.prefix && name.local_name != u"xmlns") {
      prefix = u"xmlns";
    }
  } else if (name.namespace_uri == html_names::kXmlNamespace) {
    prefix = u"xml";
  } else if (name.namespace_uri == html_names::kXlinkNamespace) {
    prefix = u"xlink";
  }
  if (!prefix || prefix->empty()) {
    return name.local_name;
  }
  return *prefix + u":" + name.local_name;
}

// The node whose children an element's markup holds: a template's contents.
const Node& ChildrenHolder(const Node& node) {
  if (node.IsElementNode() &&
      static_cast<const Element&>(node).HasHTMLTagName(u"template")) {
    return *static_cast<const HTMLTemplateElement&>(node).Content();
  }
  return node;
}

void AppendElement(std::u16string& markup, const Element& element) {
  // The qualified name, whatever the namespace, as Blink's
  // MarkupFormatter::AppendStartTagOpen() writes it, where the HTML Standard
  // would write the local name of an HTML, SVG or MathML element.
  const std::u16string tag_name = element.TagQName().ToString();
  markup += u'<';
  markup += tag_name;
  const auto append_attribute = [&markup](std::u16string_view name,
                                          const std::u16string& value) {
    markup += u' ';
    markup += name;
    markup += u"=\"";
    AppendEscaped(markup, value, /*in_attribute=*/true);
    markup += u'"';
  };
  // The is value the element was made with, when no is attribute says it.
  if (element.IsValue() && !element.FindAttribute(u"is")) {
    append_attribute(u"is", *element.IsValue());
  }
  for (const Element::Attribute& attribute : element.attributes()) {
    append_attribute(SerializedAttributeName(attribute.name), attribute.value);
  }
  markup += u'>';
  if (IsVoidElement(element)) {
    return;
  }
  AppendChildren(markup, ChildrenHolder(element));
  markup += u"</";
  markup += tag_name;
  markup += u'>';
}

void AppendNode(std::u16string& markup, const Node& node) {
  switch (node.GetNodeType()) {
    case Node::kElementNode:
      AppendElement(markup, static_cast<const Element&>(node));
      break;
    case Node::kTextNode: {
      const std::u16string& data =
          static_cast<const CharacterData&>(node).Data();
      if (HoldsRawText(node)) {
        markup += data;
      } else {
        AppendEscaped(markup, data, /*in_attribute=*/false);
      }
      break;
    }
    case Node::kCommentNode:
      markup += u"<!--";
      markup += static_cast<const CharacterData&>(node).Data();
      markup += u"-->";
      break;
    case Node::kDocumentTypeNode:
      markup += u"<!DOCTYPE ";
      markup += static_cast<const DocumentType&>(node).Name();
      markup += u'>';
      break;
    case Node::kDocumentNode:
    case Node::kDocumentFragmentNode:
      AppendChildren(markup, node);
      break;
    default:
      break;
  }
}

}  // namespace

std::u16string SerializeChildren(const Node& node) {
  // MarkupAccumulator::SerializeNodesWithNamespaces() writes no children of an
  // element that has no end tag, whose markup it is too.
  if (node.IsElementNode() &&
      IsVoidElement(static_cast<const Element&>(node))) {
    return std::u16string();
  }
  std::u16string markup;
  AppendChildren(markup, ChildrenHolder(node));
  return markup;
}

std::u16string SerializeNode(const Node& node) {
  std::u16string markup;
  AppendNode(markup, node);
  return markup;
}

}  // namespace mitto
