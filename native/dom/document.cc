#include "mitto/dom/document.h"

#include <algorithm>
#include <variant>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_document.h"
#include "mitto/dom/attached_traversals.h"
#include "mitto/dom/attr.h"
#include "mitto/dom/child_list_mutation_scope.h"
#include "mitto/dom/comment.h"
#include "mitto/dom/cookie_jar.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/event.h"
#include "mitto/dom/html_all_collection.h"
#include "mitto/dom/html_document.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_element_factory.h"
#include "mitto/dom/html_head_element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/mathml_element.h"
#include "mitto/dom/node_iterator.h"
#include "mitto/dom/node_lists_node_data.h"
#include "mitto/dom/permissions_policy.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/range.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/svg_element.h"
#include "mitto/dom/svg_element_factory.h"
#include "mitto/dom/text.h"
#include "mitto/dom/tree_walker.h"
#include "mitto/dom/window.h"
#include "mitto/encoding/text_codec.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "mitto/encoding/url_with_encoding.h"
#include "third_party/icu/source/i18n/unicode/gregocal.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {

namespace {

void ThrowInvalidTagName(const std::u16string& name,
                         bindings::ExceptionState& exception_state) {
  exception_state.ThrowDOMException(
      "InvalidCharacterError",
      base::StrCat({"The tag name provided ('", base::UTF16ToUTF8(name),
                    "') is not a valid name."}));
}

// Gives `element` the is value of createElement()'s or createElementNS()'s
// options. With no custom element definitions, that leaves an HTML element
// undefined. A string in place of the options gives none, as Blink's
// FlattenCreateElementOptions().
void SetIsValueFromOptions(
    Element& element,
    const bindings::DOMStringOrElementCreationOptions& options) {
  const auto* const dictionary =
      std::get_if<bindings::ElementCreationOptions>(&options);
  if (dictionary && dictionary->is) {
    element.SetIsValue(*dictionary->is);
  }
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
  visitor->Trace(images_);
  visitor->Trace(embeds_);
  visitor->Trace(links_);
  visitor->Trace(forms_);
  visitor->Trace(scripts_);
  visitor->Trace(anchors_);
  visitor->Trace(applets_);
  visitor->Trace(all_);
  visitor->Trace(feature_policy_);
  visitor->Trace(template_document_);
  visitor->Trace(template_document_host_);
  visitor->Trace(traversals_);
}

mitto::Location* Document::Location() const {
  return window_ ? window_->Location() : nullptr;
}

// SecurityOrigin::Domain(): the host, which an opaque origin lacks.
std::u16string Document::Domain() const {
  const url::Origin origin = url::Origin::Create(GURL(url_));
  return origin.opaque() ? std::u16string() : base::UTF8ToUTF16(origin.host());
}

// Blink's setDesignMode(): "on" or "off" in any case, and nothing for others.
void Document::SetDesignMode(const std::u16string& value) {
  if (base::EqualsCaseInsensitiveASCII(value, u"on")) {
    design_mode_ = true;
  } else if (base::EqualsCaseInsensitiveASCII(value, u"off")) {
    design_mode_ = false;
  }
}

// Blink's ScrollingElementNoLayout(): the root element in standards mode, and
// in quirks mode the body, which nothing lays out as a scroll container.
Element* Document::ScrollingElement() const {
  if (!InQuirksMode()) {
    return DocumentElement();
  }
  HTMLElement* const body = Body();
  return body && body->HasHTMLTagName(u"body") ? body : nullptr;
}

// With no element focused, the body, as Blink's Document::ActiveElement()
// gives it.
Element* Document::ActiveElement() const {
  return Body();
}

// Blink's lastModified(): the response's Last-Modified, or else now, exploded
// in local time, which for a Page is that of its time zone, ICU's default, in
// the Gregorian calendar whatever the locale.
std::u16string Document::LastModified() const {
  const base::Time time = last_modified_.value_or(base::Time::Now());
  UErrorCode status = U_ZERO_ERROR;
  icu::GregorianCalendar calendar(status);
  calendar.setTime(time.InMillisecondsFSinceUnixEpoch(), status);
  const auto field = [&](UCalendarDateFields name) {
    return calendar.get(name, status);
  };
  const std::string text = base::StringPrintf(
      "%02d/%02d/%04d %02d:%02d:%02d", field(UCAL_MONTH) + 1, field(UCAL_DATE),
      field(UCAL_YEAR), field(UCAL_HOUR_OF_DAY), field(UCAL_MINUTE),
      field(UCAL_SECOND));
  return U_SUCCESS(status) ? base::ASCIIToUTF16(text) : std::u16string();
}

mitto::CustomElementRegistry* Document::CustomElementRegistry() const {
  return window_ ? window_->CustomElements() : nullptr;
}

HTMLCollection* Document::Collection(cppgc::Member<HTMLCollection>& cached,
                                     HTMLCollection::Type type) {
  if (!cached) {
    cached = HTMLCollection::Create(this, type);
  }
  return cached.Get();
}

HTMLCollection* Document::Images() {
  return Collection(images_, HTMLCollection::Type::kDocImages);
}

HTMLCollection* Document::Embeds() {
  return Collection(embeds_, HTMLCollection::Type::kDocEmbeds);
}

HTMLCollection* Document::Links() {
  return Collection(links_, HTMLCollection::Type::kDocLinks);
}

HTMLCollection* Document::Forms() {
  return Collection(forms_, HTMLCollection::Type::kDocForms);
}

HTMLCollection* Document::Scripts() {
  return Collection(scripts_, HTMLCollection::Type::kDocScripts);
}

HTMLCollection* Document::Anchors() {
  return Collection(anchors_, HTMLCollection::Type::kDocAnchors);
}

HTMLCollection* Document::Applets() {
  return Collection(applets_, HTMLCollection::Type::kDocApplets);
}

HTMLAllCollection* Document::All() {
  if (!all_) {
    all_ = HTMLAllCollection::Create(this);
  }
  return all_.Get();
}

// Blink's Document::cookie() and setCookie(), for a document that cookies are
// enabled for, and whose origin can have cookies exactly when the Page
// program has given it a jar.
// Upstream: third_party/blink/renderer/core/dom/document.cc
std::u16string Document::Cookie(
    bindings::ExceptionState& exception_state) const {
  if (!window_) {
    return std::u16string();
  }
  if (!cookie_jar_) {
    ThrowCookiesDenied(exception_state);
    return std::u16string();
  }
  return cookie_jar_->Cookies();
}

void Document::SetCookie(const std::u16string& value,
                         bindings::ExceptionState& exception_state) {
  if (!window_) {
    return;
  }
  if (!cookie_jar_) {
    ThrowCookiesDenied(exception_state);
    return;
  }
  cookie_jar_->SetCookie(value);
}

void Document::ThrowCookiesDenied(
    bindings::ExceptionState& exception_state) const {
  exception_state.ThrowDOMException(
      "SecurityError", url_.starts_with(u"data:")
                           ? "Cookies are disabled inside 'data:' URLs."
                           : "Access is denied for this document.");
}

std::u16string Document::ReadyState() const {
  switch (readiness_) {
    case Readiness::kLoading:
      return u"loading";
    case Readiness::kInteractive:
      return u"interactive";
    case Readiness::kComplete:
      return u"complete";
  }
  NOTREACHED();
}

v8::Local<v8::Promise> Document::HasStorageAccess() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
           .ToLocal(&resolver)) {
    return v8::Local<v8::Promise>();
  }
  // Blink answers false outside a secure context, and otherwise whether the
  // document's cookies are enabled, which they are in a new profile.
  const bool access = window_ && window_->IsSecureContextForBindings();
  ResolveFromTask(isolate, resolver, v8::Boolean::New(isolate, access));
  return resolver->GetPromise();
}

PermissionsPolicy* Document::FeaturePolicy() {
  if (!feature_policy_ && window_) {
    feature_policy_ = PermissionsPolicy::Create(
        url_, IsUnloadDeprecated(window_->Start().features.deprecated_unload,
                                 GURL(url_)));
  }
  return feature_policy_.Get();
}

void Document::SetReadiness(Readiness readiness) {
  if (readiness == readiness_) {
    return;
  }
  readiness_ = readiness;
  if (readiness == Readiness::kLoading) {
    return;
  }
  bindings::EventInit init;
  Event* const event = Event::Create(u"readystatechange", init);
  event->SetTrusted(true);
  DispatchEvent(event);
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

std::u16string Document::CharacterSet() const {
  return base::ASCIIToUTF16(encoding_);
}

void Document::SetEncoding(std::string_view encoding,
                           bool detected_heuristically) {
  DCHECK(!encoding.empty());
  // The loader sets the encoding after each piece of the body, so the tree is
  // searched only when windows-1252 gives way to another.
  Element* title_element = nullptr;
  if (encoding_ != encoding && encoding_ == kWindows1252EncodingName) {
    // The first title element in tree order, as Blink keeps its title element.
    for (Node* node = FirstChild(); node; node = node->NextInTree(this)) {
      if (node->IsElementNode() &&
          static_cast<Element*>(node)->HasHTMLTagName(u"title")) {
        title_element = static_cast<Element*>(node);
        break;
      }
    }
  }
  if (title_element && !title_element->FirstElementChild()) {
    const std::u16string text = title_element->DescendantTextContent();
    if (std::ranges::all_of(text, [](char16_t c) { return c <= 0xFF; })) {
      std::string original_bytes(text.begin(), text.end());
      bool saw_error = false;
      const std::u16string decoded = NewTextCodec(encoding)->Decode(
          base::as_byte_span(original_bytes), FlushBehavior::kDataEof,
          /*stop_on_error=*/false, saw_error);
      // textContent's setter: one Text node, or none for an empty string.
      title_element->RemoveChildren();
      if (!decoded.empty()) {
        title_element->ParserAppendChild(Text::Create(*this, decoded));
      }
    }
  }
  encoding_ = std::string(encoding);
  encoding_detected_heuristically_ = detected_heuristically;
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
  // HTMLTitleElement's text setter: replace all with one Text node, one
  // record for observers.
  const ChildListMutationScope mutation(*title_element);
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

Attr* Document::CreateAttribute(const std::u16string& local_name,
                                bindings::ExceptionState& exception_state) {
  if (!IsValidAttributeLocalName(local_name)) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        base::StrCat({"The localName provided ('",
                      base::UTF16ToUTF8(local_name),
                      "') contains an invalid character."}));
    return nullptr;
  }
  return Attr::Create(
      *this,
      QualifiedName{.local_name =
                        is_html_ ? base::ToLowerASCII(local_name) : local_name},
      std::u16string());
}

Attr* Document::CreateAttributeNS(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& qualified_name,
    bindings::ExceptionState& exception_state) {
  const std::optional<QualifiedName> name = QualifiedName::Parse(
      namespace_uri, qualified_name, QualifiedName::ParsingMode::kAttribute,
      exception_state);
  if (!name) {
    return nullptr;
  }
  if (!name->HasValidNamespace()) {
    exception_state.ThrowDOMException(
        "NamespaceError",
        base::StrCat({"The namespace URI provided ('",
                      base::UTF16ToUTF8(namespace_uri.value_or(u"")),
                      "') is not valid for the qualified name provided ('",
                      base::UTF16ToUTF8(qualified_name), "')."}));
    return nullptr;
  }
  return Attr::Create(*this, *name, std::u16string());
}

DocumentFragment* Document::CreateDocumentFragment() {
  return DocumentFragment::Create(*this);
}

Element* Document::CreateElementNS(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& qualified_name,
    bindings::ExceptionState& exception_state) {
  // Blink's CreateQualifiedName(), then CreateRawElement().
  const std::optional<QualifiedName> name = QualifiedName::Parse(
      namespace_uri, qualified_name, QualifiedName::ParsingMode::kElement,
      exception_state);
  if (!name) {
    return nullptr;
  }
  if (!name->HasValidNamespace()) {
    exception_state.ThrowDOMException(
        "NamespaceError",
        base::StrCat({"The namespace URI provided ('",
                      base::UTF16ToUTF8(namespace_uri.value_or(u"")),
                      "') is not valid for the qualified name provided ('",
                      base::UTF16ToUTF8(qualified_name), "')."}));
    return nullptr;
  }
  return CreateElement(name->namespace_uri, name->prefix, name->local_name);
}

Element* Document::CreateElementNS(
    const std::optional<std::u16string>& namespace_uri,
    const std::u16string& qualified_name,
    const bindings::DOMStringOrElementCreationOptions& options,
    bindings::ExceptionState& exception_state) {
  const std::optional<QualifiedName> name = QualifiedName::Parse(
      namespace_uri, qualified_name, QualifiedName::ParsingMode::kElement,
      exception_state);
  if (!name) {
    return nullptr;
  }
  if (!name->HasValidNamespace()) {
    exception_state.ThrowDOMException(
        "NamespaceError",
        base::StrCat({"The namespace URI provided ('",
                      base::UTF16ToUTF8(namespace_uri.value_or(u"")),
                      "') is not valid for the qualified name provided ('",
                      base::UTF16ToUTF8(qualified_name), "')."}));
    return nullptr;
  }
  // Blink then checks the whole name.
  if (!IsValidElementLocalName(qualified_name)) {
    ThrowInvalidTagName(qualified_name, exception_state);
    return nullptr;
  }
  Element* const element =
      CreateElement(name->namespace_uri, name->prefix, name->local_name);
  SetIsValueFromOptions(*element, options);
  return element;
}

Element* Document::CreateElementForBinding(
    const std::u16string& local_name,
    bindings::ExceptionState& exception_state) {
  if (!IsValidElementLocalName(local_name)) {
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
  Element* const element = CreateElementForBinding(local_name, exception_state);
  if (element) {
    SetIsValueFromOptions(*element, options);
  }
  return element;
}

Node* Document::ImportNode(Node* node,
                           const bindings::BooleanOrImportNodeOptions& options,
                           bindings::ExceptionState& exception_state) {
  // Blink's importNode(): a boolean says whether to copy the subtree, and
  // options say so unless they ask for the node alone. A custom element
  // registry, which the Runtime has none of, changes nothing.
  bool deep = false;
  if (const bool* subtree = std::get_if<bool>(&options)) {
    deep = *subtree;
  } else {
    deep = !std::get<bindings::ImportNodeOptions>(options).self_only;
  }
  if (node->IsDocumentNode()) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        "The node provided is a document, which may not be imported.");
    return nullptr;
  }
  return node->Clone(*this, deep);
}

Node* Document::AdoptNode(Node* node,
                          bindings::ExceptionState& exception_state) {
  // Blink's adoptNode().
  if (node->IsDocumentNode()) {
    exception_state.ThrowDOMException(
        "NotSupportedError", base::StrCat({"The node provided is of type '",
                                           base::UTF16ToUTF8(node->NodeName()),
                                           "', which may not be adopted."}));
    return nullptr;
  }
  if (node->GetNodeType() == kAttributeNode) {
    Attr* const attr = static_cast<Attr*>(node);
    if (Element* const owner = attr->OwnerElement()) {
      owner->RemoveAttributeNode(attr, exception_state);
    }
  } else if (Node* const parent = node->ParentNode()) {
    parent->RemoveChild(node, exception_state);
    if (exception_state.HadException()) {
      return nullptr;
    }
  }
  node->SetTreeDocument(*this);
  return node;
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

NodeList* Document::GetElementsByName(const std::u16string& name) {
  return EnsureNodeLists().NameNodeList(*this, name);
}

Range* Document::CreateRange() {
  return Range::Create(*this);
}

NodeIterator* Document::CreateNodeIterator(Node* root,
                                           uint32_t what_to_show,
                                           v8::Local<v8::Object> filter) {
  return NodeIterator::Create(root, what_to_show, filter);
}

TreeWalker* Document::CreateTreeWalker(Node* root,
                                       uint32_t what_to_show,
                                       v8::Local<v8::Object> filter) {
  return TreeWalker::Create(root, what_to_show, filter);
}

AttachedTraversals& Document::Traversals() {
  if (!traversals_) {
    traversals_ = bindings::MakeGarbageCollected<AttachedTraversals>();
  }
  return *traversals_;
}

Element* Document::CreateElement(
    const std::optional<std::u16string>& namespace_uri,
    const std::optional<std::u16string>& prefix,
    const std::u16string& local_name) {
  Element* element = nullptr;
  if (namespace_uri == html_names::kXhtmlNamespace) {
    element = CreateHTMLElement(*this, local_name);
  } else if (namespace_uri == html_names::kSvgNamespace) {
    element = CreateSVGElement(*this, local_name);
  } else if (namespace_uri == html_names::kMathMlNamespace) {
    element = MathMLElement::Create(*this, local_name);
  } else {
    return Element::Create(*this, namespace_uri, prefix, local_name);
  }
  if (prefix) {
    element->SetPrefixForCreateElementNS(prefix);
  }
  return element;
}

Document& Document::EnsureTemplateDocument() {
  if (template_document_host_) {
    return *this;
  }
  if (!template_document_) {
    template_document_ = is_html_ ? CreateHTML() : Create();
    template_document_->template_document_host_ = this;
  }
  return *template_document_;
}

Node* Document::CloneWithoutChildren(Document& document) const {
  Document* const copy = is_html_ ? CreateHTML() : Create();
  copy->SetCompatibilityMode(compat_mode_);
  // Blink's CloneDataFromDocument() copies the encoding too.
  copy->encoding_ = encoding_;
  copy->encoding_detected_heuristically_ = encoding_detected_heuristically_;
  return copy;
}

uint16_t Document::GetNodeType() const {
  return kDocumentNode;
}

std::u16string Document::NodeName() const {
  return u"#document";
}

void Document::SetUrl(std::u16string url) {
  url_ = std::move(url);
  UpdateBaseUrl();
}

const GURL& Document::BaseUrl() const {
  static const base::NoDestructor<GURL> kBlank("about:blank");
  return base_url_.is_empty() ? *kBlank : base_url_;
}

GURL Document::CompleteUrl(std::u16string_view url) const {
  // A document with no base URL resolves only an absolute URL, whose query is
  // still in the document's encoding.
  return ResolveUrlWithEncoding(base_url_, url, encoding_);
}

void Document::ProcessBaseElement() {
  // The first href of a base element of the HTML namespace, in tree order.
  const std::u16string* href = nullptr;
  for (Node* node = FirstChild(); node && !href;
       node = node->NextInTree(this)) {
    if (node->IsElementNode()) {
      const auto& element = static_cast<const Element&>(*node);
      if (element.HasHTMLTagName(u"base")) {
        href = element.FindAttribute(u"href");
      }
    }
  }
  GURL base_element_url;
  if (href) {
    const std::u16string_view stripped =
        base::TrimString(*href, u" \t\n\f\r", base::TRIM_ALL);
    if (!stripped.empty()) {
      base_element_url = FallbackBaseUrl().Resolve(stripped);
      // A URL that is not valid keeps what it was given, as a KURL does.
      if (!base_element_url.is_valid()) {
        base_element_url = GURL(stripped);
      }
    }
  }
  const bool data_or_javascript = base_element_url.SchemeIs("data") ||
                                  base_element_url.SchemeIs("javascript");
  if (data_or_javascript && window_) {
    window_->AddConsoleMessage(
        ConsoleLevel::kError,
        base::StrCat({"'", base_element_url.scheme(),
                      "' URLs may not be used as base URLs for a document."}));
  }
  if (base_element_url == base_element_url_) {
    return;
  }
  // The HTML Standard's frozen base URL: the fallback base URL for a URL of
  // data or javascript, or for a document whose scripts cannot run.
  base_element_url_ =
      !data_or_javascript && window_ ? base_element_url : FallbackBaseUrl();
  UpdateBaseUrl();
}

void Document::UpdateBaseUrl() {
  base_url_ =
      !base_element_url_.is_empty() ? base_element_url_ : FallbackBaseUrl();
  if (!base_url_.is_valid()) {
    base_url_ = GURL();
  }
}

}  // namespace mitto
