#include "mitto/dom/xml_dom_builder.h"

#include <utility>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/cdata_section.h"
#include "mitto/dom/comment.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/html_template_element.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/processing_instruction.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/svg_elements.h"
#include "mitto/dom/text.h"
#include "mitto/dom/xml_tree_viewer.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

namespace {

// HTMLConstructionSite's limit is 512; Blink's XML parsers' is this.
constexpr size_t kMaxXmlTreeDepth = 5000;

bool IsHtmlScript(const Element& element) {
  return element.HasHTMLTagName(u"script");
}

bool IsSvgScript(const Element& element) {
  return element.NamespaceURI() == html_names::kSvgNamespace &&
         element.LocalName() == u"script";
}

// Gives up a fragment's parse with an error that the call that parses it
// throws, as Blink's PassThroughException rethrows it, or one that a
// document's parse ignores.
void RethrowInFragment(v8::TryCatch& try_catch, bool fragment) {
  if (fragment && try_catch.HasCaught()) {
    try_catch.ReThrow();
  }
}

}  // namespace

XmlDomBuilder::XmlDomBuilder(HtmlTreeBuilderReferences& references,
                             Node* fragment,
                             Element* context_element,
                             bool scripts_already_started)
    : references_(references),
      fragment_(fragment),
      scripts_already_started_(scripts_already_started),
      current_node_(fragment ? fragment
                             : static_cast<Node*>(&references.document())) {
  if (fragment) {
    references_->Keep(fragment);
  }
  // The HTML Standard's XML fragment parsing algorithm, step 2: the prefixes
  // and the default namespace in scope on the context, the element's own
  // name's above its xmlns attributes, outermost first.
  std::vector<Element*> elements;
  for (Element* element = context_element; element;
       element = element->ParentElement()) {
    elements.push_back(element);
  }
  for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
    Element* const element = *it;
    for (const Element::Attribute& attribute : element->attributes()) {
      if (attribute.name.local_name == u"xmlns") {
        default_namespace_uri_ = attribute.value;
      } else if (attribute.name.prefix == u"xmlns") {
        prefix_to_namespace_[attribute.name.local_name] = attribute.value;
      }
    }
    if (!element->NamespaceURI()) {
      continue;
    }
    if (!element->Prefix()) {
      default_namespace_uri_ = element->NamespaceURI();
    } else {
      prefix_to_namespace_[*element->Prefix()] = *element->NamespaceURI();
    }
  }
}

XmlDomBuilder::~XmlDomBuilder() = default;

Document& XmlDomBuilder::document() const {
  return references_->document();
}

void XmlDomBuilder::HandleError(XmlErrors::Type type,
                                std::string_view message,
                                MarkupPosition where) {
  errors_.HandleError(type, message, where);
  if (type != XmlErrors::Type::kWarning) {
    saw_error_ = true;
  }
  if (type == XmlErrors::Type::kFatal) {
    StopParsing();
  }
}

void XmlDomBuilder::StopParsing() {
  if (stopped_) {
    return;
  }
  // The block goes in at once, before the text that waits, which never
  // does.
  if (saw_error_ && !fragment_) {
    errors_.InsertErrorMessageBlock(document());
  }
  stopped_ = true;
}

void XmlDomBuilder::StartDocument(const std::optional<std::u16string>& version,
                                  const std::optional<std::u16string>& encoding,
                                  Standalone standalone) {
  if (standalone == Standalone::kNoXmlDeclaration) {
    document().SetHasXMLDeclaration(false);
    return;
  }
  bindings::ExceptionState exception_state(v8::Isolate::GetCurrent());
  // A version other than 1.0 is read as 1.0, as an XML 1.0 processor does.
  if (version == u"1.0") {
    document().SetXmlVersion(version, exception_state);
  }
  if (standalone != Standalone::kUnspecified) {
    document().SetXmlStandalone(standalone == Standalone::kYes,
                                exception_state);
  }
  if (encoding) {
    document().SetXmlEncoding(encoding);
  }
  document().SetHasXMLDeclaration(true);
}

void XmlDomBuilder::ProcessingInstruction(const std::u16string& target,
                                          const std::u16string& data) {
  if (stopped_ || !UpdateLeafTextNode()) {
    return;
  }
  // A target or data that the reader takes and Blink's checks refuse is left
  // out, with no error.
  mitto::ProcessingInstruction* pi = nullptr;
  {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::TryCatch try_catch(isolate);
    bindings::ExceptionState exception_state(isolate);
    pi = current_node_->GetDocument().CreateProcessingInstruction(
        target, data, exception_state);
  }
  if (!pi) {
    return;
  }
  current_node_->ParserAppendChild(pi);
  // ProcessingInstruction::CheckStyleSheet(): a style sheet's processing
  // instruction in the document of a frame is CSS, or XSLT for an XML type,
  // either of which keeps the XML tree viewer away. Neither loads yet.
  if (target == u"xml-stylesheet" && !fragment_ && document().DefaultView() &&
      pi->ParentNode() == &document()) {
    const std::u16string type = pi->GetAttribute(u"type").value_or(u"");
    if (type.empty() || type == u"text/css") {
      saw_css_ = true;
    } else if (type == u"text/xml" || type == u"text/xsl" ||
               type == u"application/xml" || type == u"application/xhtml+xml" ||
               type == u"application/rss+xml" ||
               type == u"application/atom+xml") {
      saw_xsl_ = true;
    }
  }
}

void XmlDomBuilder::StartElement(
    const std::u16string& local_name,
    const std::optional<std::u16string>& prefix,
    const std::optional<std::u16string>& namespace_uri,
    const std::vector<NamespaceDeclaration>& namespaces,
    const std::vector<ReadAttribute>& attributes,
    MarkupPosition position) {
  if (stopped_ || !UpdateLeafTextNode()) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();

  // The namespace declarations first, then the attributes, as Blink sets
  // them: Element::ParseAttributeName() for each.
  std::vector<Element::Attribute> prefixed_attributes;
  bool encountered_namespace_reset = false;
  for (const NamespaceDeclaration& declaration : namespaces) {
    const std::u16string name =
        declaration.prefix ? u"xmlns:" + *declaration.prefix : u"xmlns";
    const v8::TryCatch try_catch(isolate);
    bindings::ExceptionState exception_state(isolate);
    std::optional<QualifiedName> parsed = QualifiedName::Parse(
        html_names::kXmlnsNamespace, name,
        QualifiedName::ParsingMode::kAttribute, exception_state);
    if (!parsed || !parsed->HasValidNamespace()) {
      StopParsing();
      return;
    }
    if (parsed->local_name == u"xmlns") {
      encountered_namespace_reset = declaration.uri.empty();
    }
    prefixed_attributes.push_back({std::move(*parsed), declaration.uri});
  }

  // In a fragment, an element of no namespace takes what its context has in
  // scope, unless a declaration undid the default namespace.
  std::optional<std::u16string> adjusted_uri = namespace_uri;
  if (fragment_ && !adjusted_uri) {
    if (prefix) {
      if (const auto it = prefix_to_namespace_.find(*prefix);
          it != prefix_to_namespace_.end()) {
        adjusted_uri = it->second;
      }
    } else if (!encountered_namespace_reset && !ancestor_resetting_namespace_) {
      adjusted_uri = default_namespace_uri_;
    }
  }
  if (adjusted_uri && adjusted_uri->empty()) {
    adjusted_uri.reset();
  }

  for (const ReadAttribute& attribute : attributes) {
    v8::TryCatch try_catch(isolate);
    bindings::ExceptionState exception_state(isolate);
    std::optional<QualifiedName> parsed = QualifiedName::Parse(
        attribute.namespace_uri, attribute.qualified_name,
        QualifiedName::ParsingMode::kAttribute, exception_state);
    if (parsed && !parsed->HasValidNamespace()) {
      exception_state.ThrowDOMException(
          "NamespaceError",
          base::StrCat(
              {"'", base::UTF16ToUTF8(attribute.namespace_uri.value_or(u"")),
               "' is an invalid namespace for attributes."}));
      parsed.reset();
    }
    if (!parsed) {
      StopParsing();
      RethrowInFragment(try_catch, fragment_);
      return;
    }
    prefixed_attributes.push_back({std::move(*parsed), attribute.value});
  }

  Document& node_document = current_node_->GetDocument();
  Element* element = nullptr;
  if (prefix && !prefix->empty() && !adjusted_uri) {
    // A prefix with no namespace is part of the local name.
    element = node_document.CreateElement(std::nullopt, std::nullopt,
                                          *prefix + u":" + local_name);
  } else {
    element = node_document.CreateElement(
        adjusted_uri,
        prefix && !prefix->empty() ? prefix : std::optional<std::u16string>(),
        local_name);
  }
  for (const Element::Attribute& attribute : prefixed_attributes) {
    element->SetAttributeWithoutValidation(attribute.name, attribute.value);
  }
  if (fragment_ && encountered_namespace_reset &&
      !ancestor_resetting_namespace_) {
    ancestor_resetting_namespace_ = element;
    references_->Keep(element);
  }
  // CreateElementFlags::ByParser() or ByFragmentParser(): a script element is
  // the parser's, already started where its scripts never run, and its text
  // begins where the reader is.
  if (IsHtmlScript(*element)) {
    auto* const script = static_cast<HTMLScriptElement*>(element);
    script->SetCreatedByParser(node_document);
    if (scripts_already_started_) {
      script->SetAlreadyStarted();
    }
    script->SetStartPosition(position.line, position.column);
  } else if (IsSvgScript(*element)) {
    auto* const script = static_cast<SVGScriptElement*>(element);
    script->SetCreatedByParser(node_document);
    if (scripts_already_started_) {
      script->SetAlreadyStarted();
    }
    script->SetStartPosition(position.line, position.column);
  }
  current_node_->ParserAppendChild(element);
  if (stopped_) {
    return;
  }
  if (element->HasHTMLTagName(u"template")) {
    PushCurrentNode(static_cast<HTMLTemplateElement*>(element)->Content(),
                    position);
  } else {
    PushCurrentNode(element, position);
  }
}

Element* XmlDomBuilder::EndElement() {
  if (stopped_ || !UpdateLeafTextNode()) {
    return nullptr;
  }
  Node* const node = current_node_;
  if (!node || !node->IsElementNode()) {
    // A template's contents, which ends the template that undid the default
    // namespace.
    if (ancestor_resetting_namespace_ &&
        static_cast<Element*>(ancestor_resetting_namespace_.get())
            ->HasHTMLTagName(u"template") &&
        static_cast<HTMLTemplateElement*>(ancestor_resetting_namespace_.get())
                ->Content() == node) {
      ancestor_resetting_namespace_ = nullptr;
    }
    PopCurrentNode();
    return nullptr;
  }
  if (ancestor_resetting_namespace_ == node) {
    ancestor_resetting_namespace_ = nullptr;
  }
  auto* const element = static_cast<Element*>(node);
  PopCurrentNode();
  // A script runs only where scripts run, and only while it is connected;
  // the parser processes it before it reads on.
  if (scripts_already_started_ || !element->IsConnected()) {
    return nullptr;
  }
  return IsHtmlScript(*element) || IsSvgScript(*element) ? element : nullptr;
}

void XmlDomBuilder::Characters(std::u16string_view text) {
  if (stopped_) {
    return;
  }
  CreateLeafTextNodeIfNeeded();
  buffered_text_ += text;
}

void XmlDomBuilder::CData(const std::u16string& data) {
  if (stopped_ || !UpdateLeafTextNode()) {
    return;
  }
  current_node_->ParserAppendChild(
      CDATASection::Create(current_node_->GetDocument(), data));
}

void XmlDomBuilder::Comment(const std::u16string& data) {
  if (stopped_ || !UpdateLeafTextNode()) {
    return;
  }
  current_node_->ParserAppendChild(
      mitto::Comment::Create(current_node_->GetDocument(), data));
}

void XmlDomBuilder::DocType(const std::u16string& name,
                            const std::u16string& public_id,
                            const std::u16string& system_id) {
  if (stopped_ || !UpdateLeafTextNode()) {
    return;
  }
  document().ParserAppendChild(
      DocumentType::Create(document(), name, public_id, system_id));
}

void XmlDomBuilder::EndDocument() {
  UpdateLeafTextNode();
}

void XmlDomBuilder::End() {
  // DoEnd(): a document that showed no error, no style sheet and no XSLT,
  // with no element that a known namespace styles, gets the XML tree viewer.
  if (!fragment_ && !saw_error_ && !saw_css_ && !saw_xsl_ &&
      HasNoStyleInformation(document())) {
    TransformDocumentToXmlTreeView(document());
  }
  if (saw_error_ && !stopped_ && !fragment_) {
    errors_.InsertErrorMessageBlock(document());
  } else {
    UpdateLeafTextNode();
  }
  current_node_ = nullptr;
  current_node_stack_.clear();
  leaf_text_node_ = nullptr;
  ancestor_resetting_namespace_ = nullptr;
}

std::optional<std::u16string> XmlDomBuilder::FragmentNamespaceOfPrefix(
    const std::u16string& prefix) const {
  const auto it = prefix_to_namespace_.find(prefix);
  if (it == prefix_to_namespace_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void XmlDomBuilder::FailOnUndeclaredAttributePrefix(
    const std::u16string& prefix,
    const std::u16string& local_name) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::TryCatch try_catch(isolate);
  {
    bindings::ExceptionState exception_state(isolate);
    exception_state.ThrowDOMException(
        "NamespaceError",
        base::StrCat({"Namespace prefix ", base::UTF16ToUTF8(prefix),
                      " for attribute ", base::UTF16ToUTF8(local_name),
                      " is not declared."}));
  }
  StopParsing();
  RethrowInFragment(try_catch, fragment_);
}

void XmlDomBuilder::PushCurrentNode(Node* node, MarkupPosition where) {
  DCHECK(node);
  DCHECK(current_node_);
  current_node_stack_.push_back(current_node_);
  current_node_ = node;
  references_->Keep(node);
  KeepReferences();
  if (current_node_stack_.size() > kMaxXmlTreeDepth) {
    HandleError(XmlErrors::Type::kFatal, "Excessive node nesting.", where);
  }
}

void XmlDomBuilder::PopCurrentNode() {
  if (!current_node_ || current_node_stack_.empty()) {
    return;
  }
  current_node_ = current_node_stack_.back();
  current_node_stack_.pop_back();
}

void XmlDomBuilder::CreateLeafTextNodeIfNeeded() {
  if (leaf_text_node_) {
    return;
  }
  DCHECK(buffered_text_.empty());
  leaf_text_node_ = Text::Create(current_node_->GetDocument(), u"");
  references_->Keep(leaf_text_node_);
  current_node_->ParserAppendChild(leaf_text_node_);
}

bool XmlDomBuilder::UpdateLeafTextNode() {
  if (stopped_) {
    return false;
  }
  if (!leaf_text_node_) {
    return true;
  }
  leaf_text_node_->AppendData(std::exchange(buffered_text_, std::u16string()));
  leaf_text_node_ = nullptr;
  return !stopped_;
}

void XmlDomBuilder::KeepReferences() {
  // The open nodes, and nothing that has left the stack, once the references
  // have grown well past them.
  const size_t live = current_node_stack_.size() + 4;
  if (references_->size() <= 2 * live + 256) {
    return;
  }
  std::vector<Node*> nodes;
  nodes.reserve(live);
  for (const raw_ptr<Node>& node : current_node_stack_) {
    nodes.push_back(node);
  }
  for (Node* const node :
       {current_node_.get(), static_cast<Node*>(leaf_text_node_.get()),
        ancestor_resetting_namespace_.get(), fragment_.get()}) {
    if (node) {
      nodes.push_back(node);
    }
  }
  references_->KeepOnly(nodes);
}

}  // namespace mitto
