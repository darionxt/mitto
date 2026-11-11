#include "mitto/dom/markup_serializer.h"

#include <string_view>

#include "mitto/dom/character_data.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
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

// Elements whose text the serializer writes as it is. A Page runs scripts, so
// noscript is one.
bool HoldsRawText(const Node* parent) {
  if (!parent || !parent->IsElementNode()) {
    return false;
  }
  const auto& element = static_cast<const Element&>(*parent);
  if (!element.IsHTMLElement()) {
    return false;
  }
  constexpr std::u16string_view kRawTextElements[] = {
      u"style",   u"script",   u"xmp",       u"iframe",
      u"noembed", u"noframes", u"plaintext", u"noscript"};
  for (const std::u16string_view name : kRawTextElements) {
    if (element.LocalName() == name) {
      return true;
    }
  }
  return false;
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

void AppendElement(std::u16string& markup, const Element& element) {
  // The local name for an element of the HTML, SVG or MathML namespace, and
  // otherwise the qualified name.
  const std::optional<std::u16string>& namespace_uri = element.NamespaceURI();
  std::u16string tag_name = element.LocalName();
  if (element.Prefix() && namespace_uri != html_names::kXhtmlNamespace &&
      namespace_uri != html_names::kSvgNamespace &&
      namespace_uri != html_names::kMathMlNamespace) {
    tag_name = *element.Prefix() + u":" + tag_name;
  }
  markup += u'<';
  markup += tag_name;
  for (const Element::Attribute& attribute : element.attributes()) {
    markup += u' ';
    markup += attribute.name;
    markup += u"=\"";
    AppendEscaped(markup, attribute.value, /*in_attribute=*/true);
    markup += u'"';
  }
  markup += u'>';
  if (IsVoidElement(element)) {
    return;
  }
  AppendChildren(markup, element);
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
      if (HoldsRawText(node.ParentNode())) {
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
      AppendChildren(markup, node);
      break;
    default:
      break;
  }
}

}  // namespace

std::u16string SerializeChildren(const Node& node) {
  std::u16string markup;
  AppendChildren(markup, node);
  return markup;
}

std::u16string SerializeNode(const Node& node) {
  std::u16string markup;
  AppendNode(markup, node);
  return markup;
}

}  // namespace mitto
