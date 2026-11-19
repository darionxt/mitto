#include "mitto/dom/markup_serializer.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "mitto/dom/attr.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_template_element.h"
#include "mitto/dom/node.h"
#include "mitto/dom/processing_instruction.h"

namespace mitto {

namespace {

enum class SerializationType {
  kHtml,
  kXml,
};

// Blink's EntityMask: which characters are written as references.
enum EntityMask : unsigned {
  kEntityAmp = 0x0001,
  kEntityLt = 0x0002,
  kEntityGt = 0x0004,
  kEntityQuot = 0x0008,
  kEntityNbsp = 0x0010,
  kEntityTab = 0x0020,
  kEntityLineFeed = 0x0040,
  kEntityCarriageReturn = 0x0080,

  kEntityMaskInCdata = 0,
  kEntityMaskInPcdata = kEntityAmp | kEntityLt | kEntityGt,
  kEntityMaskInHtmlPcdata = kEntityMaskInPcdata | kEntityNbsp,
  kEntityMaskInAttributeValue = kEntityAmp | kEntityQuot | kEntityLt |
                                kEntityGt | kEntityTab | kEntityLineFeed |
                                kEntityCarriageReturn,
  kEntityMaskInHtmlAttributeValue =
      kEntityAmp | kEntityQuot | kEntityLt | kEntityGt | kEntityNbsp,
};

void AppendCharactersReplacingEntities(std::u16string& markup,
                                       std::u16string_view text,
                                       unsigned mask) {
  for (const char16_t c : text) {
    if (c == '&' && (mask & kEntityAmp)) {
      markup += u"&amp;";
    } else if (c == '<' && (mask & kEntityLt)) {
      markup += u"&lt;";
    } else if (c == '>' && (mask & kEntityGt)) {
      markup += u"&gt;";
    } else if (c == '"' && (mask & kEntityQuot)) {
      markup += u"&quot;";
    } else if (c == 0xA0 && (mask & kEntityNbsp)) {
      markup += u"&nbsp;";
    } else if (c == '\t' && (mask & kEntityTab)) {
      markup += u"&#9;";
    } else if (c == '\n' && (mask & kEntityLineFeed)) {
      markup += u"&#10;";
    } else if (c == '\r' && (mask & kEntityCarriageReturn)) {
      markup += u"&#13;";
    } else {
      markup += c;
    }
  }
}

void AppendAttributeValue(std::u16string& markup,
                          std::u16string_view value,
                          SerializationType type) {
  AppendCharactersReplacingEntities(markup, value,
                                    type == SerializationType::kHtml
                                        ? kEntityMaskInHtmlAttributeValue
                                        : kEntityMaskInAttributeValue);
}

// MarkupFormatter::AppendAttribute().
void AppendAttribute(std::u16string& markup,
                     const std::optional<std::u16string>& prefix,
                     std::u16string_view local_name,
                     std::u16string_view value,
                     SerializationType type) {
  markup += u' ';
  if (prefix && !prefix->empty()) {
    markup += *prefix;
    markup += u':';
  }
  markup += local_name;
  markup += u"=\"";
  AppendAttributeValue(markup, value, type);
  markup += u'"';
}

void OpenStartTag(std::u16string& markup,
                  const std::optional<std::u16string>& prefix,
                  std::u16string_view local_name) {
  markup += u'<';
  if (prefix && !prefix->empty()) {
    markup += *prefix;
    markup += u':';
  }
  markup += local_name;
}

// Blink's ElementCannotHaveEndTag(): an HTML element that
// HTMLElement::ShouldSerializeEndTag() writes no end tag for.
bool ElementCannotHaveEndTag(const Element& element) {
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

// The HTML Standard's attribute's serialized name, as Blink's
// ResolveAttributePrefixForHtml() writes it.
std::optional<std::u16string> ResolveAttributePrefixForHtml(
    const QualifiedName& name) {
  if (name.namespace_uri == html_names::kXmlnsNamespace) {
    if (!name.prefix && name.local_name != u"xmlns") {
      return u"xmlns";
    }
  } else if (name.namespace_uri == html_names::kXmlNamespace) {
    return u"xml";
  } else if (name.namespace_uri == html_names::kXlinkNamespace) {
    return u"xlink";
  }
  return name.prefix;
}

// Blink's MatchesDefaultNsDeclaration(): whether an attribute of no prefix
// declares the default namespace, by its local name for recording the
// namespace information, or by its namespace for dropping it, as
// XMLSerializerConsistentDefaultNsDeclMatching, a test feature, leaves it.
bool MatchesDefaultNsDeclarationByLocalName(const Element::Attribute& attr) {
  return !attr.name.prefix && attr.name.local_name == u"xmlns";
}

bool MatchesDefaultNsDeclarationByNamespace(const Element::Attribute& attr) {
  return !attr.name.prefix &&
         attr.name.namespace_uri == html_names::kXmlnsNamespace;
}

// Two strings equal with null taken as empty, as Blink's
// EqualIgnoringNullity() compares them.
bool EqualIgnoringNullity(const std::optional<std::u16string>& a,
                          const std::optional<std::u16string>& b) {
  return a.value_or(u"") == b.value_or(u"");
}

// The namespace prefix map and the context namespace that one element's
// children see, as Blink's MarkupAccumulator::NamespaceContext.
class NamespaceContext {
 public:
  // The DOM Parsing specification's "add", for a prefix that is not empty
  // and a namespace that may be empty, as a page can bind a prefix to no
  // namespace through the DOM.
  void Add(const std::u16string& prefix, const std::u16string& namespace_uri) {
    prefix_namespaces_[prefix] = namespace_uri;
    namespace_prefixes_[namespace_uri].push_back(prefix);
  }

  // The DOM Parsing specification's "recording the namespace information":
  // adds each prefix that the element's attributes declare, and gives the
  // default namespace that one declares, or null.
  std::optional<std::u16string> RecordNamespaceInformation(
      const Element& element) {
    std::optional<std::u16string> local_default_namespace;
    for (const Element::Attribute& attr : element.attributes()) {
      if (MatchesDefaultNsDeclarationByLocalName(attr)) {
        local_default_namespace = attr.value;
      } else if (attr.name.prefix == u"xmlns") {
        Add(attr.name.local_name, attr.value);
      }
    }
    return local_default_namespace;
  }

  // The namespace that `prefix` names, null for none or for no namespace.
  std::optional<std::u16string> LookupNamespaceUri(
      const std::optional<std::u16string>& prefix) const {
    const auto it = prefix_namespaces_.find(prefix.value_or(u""));
    if (it == prefix_namespaces_.end() || it->second.empty()) {
      return std::nullopt;
    }
    return it->second;
  }

  const std::optional<std::u16string>& ContextNamespace() const {
    return context_namespace_;
  }
  void SetContextNamespace(const std::optional<std::u16string>& context) {
    context_namespace_ = context;
  }
  void InheritLocalDefaultNamespace(
      const std::optional<std::u16string>& local_default_namespace) {
    if (!local_default_namespace) {
      return;
    }
    SetContextNamespace(local_default_namespace->empty()
                            ? std::nullopt
                            : local_default_namespace);
  }

  // The prefixes added for `ns`, in order.
  std::vector<std::u16string> PrefixList(
      const std::optional<std::u16string>& ns) const {
    const auto it = namespace_prefixes_.find(ns.value_or(u""));
    return it == namespace_prefixes_.end() ? std::vector<std::u16string>()
                                           : it->second;
  }

 private:
  std::map<std::u16string, std::u16string> prefix_namespaces_;
  std::map<std::u16string, std::vector<std::u16string>> namespace_prefixes_;
  std::optional<std::u16string> context_namespace_;
};

// Blink's MarkupAccumulator with its MarkupFormatter, which resolves no URLs
// and writes no shadow roots, as there are none yet.
class MarkupAccumulator {
 public:
  explicit MarkupAccumulator(SerializationType type) : type_(type) {}

  // MarkupAccumulator::SerializeNodes().
  std::u16string SerializeNodes(const Node& target, bool children_only) {
    if (!SerializeAsHtml()) {
      namespace_stack_.emplace_back();
      AddPrefix(u"xml", html_names::kXmlNamespace);
      prefix_index_ = 1;
    }
    SerializeNodesWithNamespaces(target, children_only);
    return std::move(markup_);
  }

 private:
  // What one element's start tag decided, as Blink's
  // ElementSerializationData.
  struct ElementSerializationData {
    bool ignore_namespace_definition_attribute = false;
    std::optional<std::u16string> serialized_prefix;
  };

  bool SerializeAsHtml() const { return type_ == SerializationType::kHtml; }

  // The node whose children an element's markup holds: a template's
  // contents.
  static const Node& ChildrenHolder(const Element& element) {
    if (element.HasHTMLTagName(u"template")) {
      return *static_cast<const HTMLTemplateElement&>(element).Content();
    }
    return element;
  }

  void SerializeNodesWithNamespaces(const Node& target, bool children_only) {
    if (!target.IsElementNode()) {
      if (!children_only) {
        AppendStartMarkup(target);
      }
      for (const Node* child = target.FirstChild(); child;
           child = child->NextSibling()) {
        SerializeNodesWithNamespaces(*child, /*children_only=*/false);
      }
      return;
    }

    const auto& element = static_cast<const Element&>(target);
    PushNamespaces();
    std::optional<std::u16string> prefix_override;
    if (!children_only) {
      prefix_override = AppendElement(element);
    }
    const bool has_end_tag =
        !(SerializeAsHtml() && ElementCannotHaveEndTag(element));
    if (has_end_tag) {
      const Node& parent = ChildrenHolder(element);
      for (const Node* child = parent.FirstChild(); child;
           child = child->NextSibling()) {
        SerializeNodesWithNamespaces(*child, /*children_only=*/false);
      }
      if (!children_only) {
        AppendEndMarkup(element, prefix_override);
      }
    }
    PopNamespaces();
  }

  // MarkupAccumulator::AppendStartMarkup() and MarkupFormatter's, for every
  // node but an element.
  void AppendStartMarkup(const Node& node) {
    switch (node.GetNodeType()) {
      case Node::kTextNode:
        AppendText(node);
        break;
      case Node::kCdataSectionNode:
        if (SerializeAsHtml()) {
          AppendText(node);
        } else {
          // Blink writes the data as it is, "]]>" too.
          markup_ += u"<![CDATA[";
          markup_ += static_cast<const CharacterData&>(node).Data();
          markup_ += u"]]>";
        }
        break;
      case Node::kCommentNode:
        markup_ += u"<!--";
        markup_ += static_cast<const CharacterData&>(node).Data();
        markup_ += u"-->";
        break;
      case Node::kProcessingInstructionNode:
        markup_ += u"<?";
        markup_ += static_cast<const ProcessingInstruction&>(node).Target();
        markup_ += u' ';
        markup_ += static_cast<const CharacterData&>(node).Data();
        markup_ += u"?>";
        break;
      case Node::kDocumentNode:
        AppendXmlDeclaration(static_cast<const Document&>(node));
        break;
      case Node::kDocumentTypeNode:
        AppendDocumentType(static_cast<const DocumentType&>(node));
        break;
      case Node::kAttributeNode:
        // Only XMLSerializer passes an attribute's node.
        AppendAttributeValue(markup_, static_cast<const Attr&>(node).Value(),
                             SerializationType::kXml);
        break;
      default:
        break;
    }
  }

  // MarkupFormatter::AppendXmlDeclaration().
  void AppendXmlDeclaration(const Document& document) {
    if (!document.HasXMLDeclaration()) {
      return;
    }
    markup_ += u"<?xml version=\"";
    markup_ += document.XmlVersion().value_or(u"");
    const std::optional<std::u16string>& encoding = document.XmlEncoding();
    if (encoding && !encoding->empty()) {
      markup_ += u"\" encoding=\"";
      markup_ += *encoding;
    }
    if (document.XmlStandaloneStatus() !=
        Document::StandaloneStatus::kStandaloneUnspecified) {
      markup_ += u"\" standalone=\"";
      markup_ += document.XmlStandalone() ? u"yes" : u"no";
    }
    markup_ += u"\"?>";
  }

  // MarkupFormatter::AppendDocumentType().
  void AppendDocumentType(const DocumentType& doctype) {
    if (doctype.Name().empty()) {
      return;
    }
    markup_ += u"<!DOCTYPE ";
    markup_ += doctype.Name();
    if (!doctype.PublicId().empty()) {
      markup_ += u" PUBLIC \"";
      markup_ += doctype.PublicId();
      markup_ += u'"';
      if (!doctype.SystemId().empty()) {
        markup_ += u" \"";
        markup_ += doctype.SystemId();
        markup_ += u'"';
      }
    } else if (!doctype.SystemId().empty()) {
      markup_ += u" SYSTEM \"";
      markup_ += doctype.SystemId();
      markup_ += u'"';
    }
    markup_ += u'>';
  }

  // MarkupFormatter::AppendText() with EntityMaskForText(): in HTML, the
  // text of these HTML elements is written as it is, and noscript's where
  // the document has a window, whose scripts a Page always runs. A document
  // of no window, such as a template's contents or one that
  // createHTMLDocument() or DOMParser made, has noscript's text escaped.
  void AppendText(const Node& text) {
    const std::u16string& data = static_cast<const CharacterData&>(text).Data();
    AppendCharactersReplacingEntities(markup_, data, EntityMaskForText(text));
  }

  unsigned EntityMaskForText(const Node& text) const {
    if (!SerializeAsHtml()) {
      return kEntityMaskInPcdata;
    }
    const Node* const parent = text.ParentNode();
    if (!parent || !parent->IsElementNode()) {
      return kEntityMaskInHtmlPcdata;
    }
    const auto& element = static_cast<const Element&>(*parent);
    if (!element.IsHTMLElement() || element.Prefix().has_value()) {
      return kEntityMaskInHtmlPcdata;
    }
    constexpr std::u16string_view kRawTextElements[] = {
        u"style",   u"script",   u"xmp",      u"iframe",
        u"noembed", u"noframes", u"plaintext"};
    for (const std::u16string_view name : kRawTextElements) {
      if (element.LocalName() == name) {
        return kEntityMaskInCdata;
      }
    }
    if (element.LocalName() == u"noscript" &&
        text.GetDocument().DefaultView() != nullptr) {
      return kEntityMaskInCdata;
    }
    return kEntityMaskInHtmlPcdata;
  }

  // MarkupAccumulator::AppendElement(): the start tag, and the prefix that
  // the end tag takes.
  std::optional<std::u16string> AppendElement(const Element& element) {
    const ElementSerializationData data = AppendStartTagOpen(element);
    if (SerializeAsHtml()) {
      // The is value the element was made with, when no is attribute says
      // it.
      if (element.IsValue() && !element.FindAttribute(u"is")) {
        AppendAttribute(markup_, std::nullopt, u"is", *element.IsValue(),
                        SerializationType::kHtml);
      }
      for (const Element::Attribute& attribute : element.attributes()) {
        AppendAttribute(markup_, ResolveAttributePrefixForHtml(attribute.name),
                        attribute.name.local_name, attribute.value,
                        SerializationType::kHtml);
      }
    } else {
      for (const Element::Attribute& attribute : element.attributes()) {
        // Drops xmlns= only where it differs from the element's namespace.
        if (data.ignore_namespace_definition_attribute &&
            MatchesDefaultNsDeclarationByNamespace(attribute) &&
            !EqualIgnoringNullity(attribute.value, element.NamespaceURI())) {
          continue;
        }
        AppendAttributeAsXmlWithNamespace(attribute);
      }
    }
    AppendStartTagClose(element);
    return data.serialized_prefix;
  }

  // MarkupAccumulator::AppendStartTagOpen(), with the DOM Parsing
  // specification's steps that it follows in XML.
  ElementSerializationData AppendStartTagOpen(const Element& element) {
    ElementSerializationData data;
    data.serialized_prefix = element.Prefix();
    if (SerializeAsHtml()) {
      OpenStartTag(markup_, element.Prefix(), element.LocalName());
      return data;
    }

    NamespaceContext& context = namespace_stack_.back();
    const std::optional<std::u16string> local_default_namespace =
        context.RecordNamespaceInformation(element);
    const std::optional<std::u16string> inherited_ns =
        context.ContextNamespace();
    const std::optional<std::u16string>& ns = element.NamespaceURI();

    // 11. The element's namespace is its parent's context namespace.
    if (inherited_ns == ns) {
      data.ignore_namespace_definition_attribute =
          local_default_namespace.has_value();
      OpenStartTag(markup_, std::nullopt, element.LocalName());
      data.serialized_prefix = std::nullopt;
      return data;
    }

    // 12. It is another.
    std::optional<std::u16string> prefix = element.Prefix();
    std::optional<std::u16string> candidate_prefix;
    if (ns && !ns->empty() && (prefix || ns != local_default_namespace)) {
      candidate_prefix = RetrievePreferredPrefixString(*ns, prefix);
    }
    // 12.4. A prefix in scope names the namespace.
    if (candidate_prefix && LookupNamespaceUri(candidate_prefix)) {
      OpenStartTag(markup_, candidate_prefix, element.LocalName());
      data.serialized_prefix = candidate_prefix;
      if (local_default_namespace != html_names::kXmlNamespace) {
        context.InheritLocalDefaultNamespace(local_default_namespace);
      }
      return data;
    }
    // 12.5. The element has a prefix of its own.
    if (prefix) {
      if (element.HasAttribute(u"xmlns:" + *prefix)) {
        prefix = GeneratePrefix(ns.value_or(u""));
      } else {
        AddPrefix(*prefix, ns.value_or(u""));
      }
      OpenStartTag(markup_, prefix, element.LocalName());
      data.serialized_prefix = prefix;
      AppendAttribute(markup_, u"xmlns", *prefix, ns.value_or(u""),
                      SerializationType::kXml);
      context.InheritLocalDefaultNamespace(local_default_namespace);
      return data;
    }
    // 12.6. No default namespace declaration matches the namespace.
    if (!local_default_namespace ||
        !EqualIgnoringNullity(local_default_namespace, ns)) {
      data.ignore_namespace_definition_attribute = true;
      context.SetContextNamespace(ns);
      OpenStartTag(markup_, element.Prefix(), element.LocalName());
      AppendAttribute(markup_, std::nullopt, u"xmlns", ns.value_or(u""),
                      SerializationType::kXml);
      return data;
    }
    // 12.7. The element's own default namespace declaration matches it.
    context.SetContextNamespace(ns);
    OpenStartTag(markup_, element.Prefix(), element.LocalName());
    return data;
  }

  // MarkupFormatter::AppendStartTagClose(), with ShouldSelfClose(): in XML,
  // an element with no children closes itself, but for an HTML element that
  // has an end tag.
  void AppendStartTagClose(const Element& element) {
    if (ShouldSelfClose(element)) {
      if (element.IsHTMLElement()) {
        // XHTML 1.0 <-> HTML compatibility.
        markup_ += u' ';
      }
      markup_ += u'/';
    }
    markup_ += u'>';
  }

  bool ShouldSelfClose(const Element& element) const {
    if (SerializeAsHtml() || element.HasChildren()) {
      return false;
    }
    return !element.IsHTMLElement() || ElementCannotHaveEndTag(element);
  }

  // MarkupFormatter::AppendEndMarkup().
  void AppendEndMarkup(const Element& element,
                       const std::optional<std::u16string>& prefix) {
    if (ShouldSelfClose(element) ||
        (!element.HasChildren() && ElementCannotHaveEndTag(element))) {
      return;
    }
    markup_ += u"</";
    if (prefix && !prefix->empty()) {
      markup_ += *prefix;
      markup_ += u':';
    }
    markup_ += element.LocalName();
    markup_ += u'>';
  }

  // MarkupAccumulator::AppendAttributeAsXmlWithNamespace(), the DOM Parsing
  // specification's serializing of an element's attributes.
  void AppendAttributeAsXmlWithNamespace(const Element::Attribute& attribute) {
    const std::optional<std::u16string>& attribute_namespace =
        attribute.name.namespace_uri;
    if (!attribute_namespace) {
      AppendAttribute(markup_, std::nullopt, attribute.name.local_name,
                      attribute.value, SerializationType::kXml);
      return;
    }
    std::optional<std::u16string> candidate_prefix =
        RetrievePreferredPrefixString(*attribute_namespace,
                                      attribute.name.prefix);
    if (*attribute_namespace == html_names::kXmlnsNamespace) {
      if (!attribute.name.prefix && attribute.name.local_name != u"xmlns") {
        candidate_prefix = u"xmlns";
      }
    } else if (ShouldAddNamespaceAttribute(attribute, candidate_prefix)) {
      if (!candidate_prefix || LookupNamespaceUri(candidate_prefix)) {
        candidate_prefix = GeneratePrefix(*attribute_namespace);
        AppendAttribute(markup_, u"xmlns", *candidate_prefix,
                        *attribute_namespace, SerializationType::kXml);
      } else {
        AppendNamespace(*candidate_prefix, *attribute_namespace);
      }
    }
    AppendAttribute(markup_, candidate_prefix, attribute.name.local_name,
                    attribute.value, SerializationType::kXml);
  }

  bool ShouldAddNamespaceAttribute(
      const Element::Attribute& attribute,
      const std::optional<std::u16string>& candidate_prefix) const {
    // An attribute without a prefix needs one generated, and a declaration
    // of it.
    if (!candidate_prefix) {
      return true;
    }
    return !EqualIgnoringNullity(LookupNamespaceUri(candidate_prefix),
                                 attribute.name.namespace_uri);
  }

  void AppendNamespace(const std::u16string& prefix,
                       const std::u16string& namespace_uri) {
    const std::optional<std::u16string> found = LookupNamespaceUri(prefix);
    if (EqualIgnoringNullity(found, namespace_uri)) {
      return;
    }
    AddPrefix(prefix, namespace_uri);
    if (prefix.empty()) {
      AppendAttribute(markup_, std::nullopt, u"xmlns", namespace_uri,
                      SerializationType::kXml);
    } else {
      AppendAttribute(markup_, u"xmlns", prefix, namespace_uri,
                      SerializationType::kXml);
    }
  }

  // The DOM Parsing specification's "retrieving a preferred prefix string",
  // as Blink has it: the preferred prefix if it is in scope for `ns`, or
  // else the last prefix added for `ns` that is still in scope for it, or
  // else the preferred prefix if nothing has it, or else null.
  std::optional<std::u16string> RetrievePreferredPrefixString(
      const std::u16string& ns,
      const std::optional<std::u16string>& preferred_prefix) const {
    const std::optional<std::u16string> ns_for_preferred =
        LookupNamespaceUri(preferred_prefix);
    if (preferred_prefix && !preferred_prefix->empty() && ns_for_preferred &&
        *ns_for_preferred == ns) {
      return preferred_prefix;
    }
    const std::vector<std::u16string> candidates =
        namespace_stack_.back().PrefixList(ns);
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
      if (EqualIgnoringNullity(LookupNamespaceUri(*it), ns)) {
        return *it;
      }
    }
    if (preferred_prefix && !preferred_prefix->empty() && !ns_for_preferred) {
      return preferred_prefix;
    }
    return std::nullopt;
  }

  void AddPrefix(const std::u16string& prefix,
                 const std::u16string& namespace_uri) {
    namespace_stack_.back().Add(prefix, namespace_uri);
  }

  std::optional<std::u16string> LookupNamespaceUri(
      const std::optional<std::u16string>& prefix) const {
    return namespace_stack_.back().LookupNamespaceUri(prefix);
  }

  // The DOM Parsing specification's "generating a prefix": ns1, ns2 and so
  // on, skipping those in scope.
  std::u16string GeneratePrefix(const std::u16string& new_namespace) {
    std::u16string generated;
    do {
      generated = u"ns" + base::NumberToString16(prefix_index_);
      ++prefix_index_;
    } while (LookupNamespaceUri(generated));
    AddPrefix(generated, new_namespace);
    return generated;
  }

  void PushNamespaces() {
    if (SerializeAsHtml()) {
      return;
    }
    NamespaceContext copy = namespace_stack_.back();
    namespace_stack_.push_back(std::move(copy));
  }

  void PopNamespaces() {
    if (SerializeAsHtml()) {
      return;
    }
    namespace_stack_.pop_back();
  }

  const SerializationType type_;
  std::u16string markup_;
  std::vector<NamespaceContext> namespace_stack_;
  int prefix_index_ = 1;
};

SerializationType SerializationTypeFor(const Node& node) {
  return node.GetDocument().IsHTMLDocument() ? SerializationType::kHtml
                                             : SerializationType::kXml;
}

}  // namespace

std::u16string SerializeChildren(const Node& node) {
  return MarkupAccumulator(SerializationTypeFor(node))
      .SerializeNodes(node, /*children_only=*/true);
}

std::u16string SerializeNode(const Node& node) {
  return MarkupAccumulator(SerializationTypeFor(node))
      .SerializeNodes(node, /*children_only=*/false);
}

std::u16string SerializeNodeAsXml(const Node& node) {
  return MarkupAccumulator(SerializationType::kXml)
      .SerializeNodes(node, /*children_only=*/false);
}

std::u16string EscapeAttributeValue(std::u16string_view value, bool for_html) {
  std::u16string escaped;
  AppendAttributeValue(
      escaped, value,
      for_html ? SerializationType::kHtml : SerializationType::kXml);
  return escaped;
}

}  // namespace mitto
