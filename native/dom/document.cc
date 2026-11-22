#include "mitto/dom/document.h"

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ref.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_document.h"
#include "mitto/bindings/v8_window.h"
#include "mitto/dom/attached_traversals.h"
#include "mitto/dom/attr.h"
#include "mitto/dom/before_unload_event.h"
#include "mitto/dom/cdata_section.h"
#include "mitto/dom/child_list_mutation_scope.h"
#include "mitto/dom/comment.h"
#include "mitto/dom/cookie_jar.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_load_timing.h"
#include "mitto/dom/document_named_properties.h"
#include "mitto/dom/document_parser.h"
#include "mitto/dom/document_script_runner.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/dom_implementation.h"
#include "mitto/dom/element.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_factory.h"
#include "mitto/dom/html_all_collection.h"
#include "mitto/dom/html_details_element.h"
#include "mitto/dom/html_document.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_element_factory.h"
#include "mitto/dom/html_head_element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_script_element.h"
#include "mitto/dom/http_refresh_scheduler.h"
#include "mitto/dom/mathml_element.h"
#include "mitto/dom/navigation_client.h"
#include "mitto/dom/node_iterator.h"
#include "mitto/dom/node_lists_node_data.h"
#include "mitto/dom/page_transition_event.h"
#include "mitto/dom/performance_timing_confidence.h"
#include "mitto/dom/permissions_policy.h"
#include "mitto/dom/processing_instruction.h"
#include "mitto/dom/qualified_name.h"
#include "mitto/dom/range.h"
#include "mitto/dom/resource_fetcher.h"
#include "mitto/dom/script_values.h"
#include "mitto/dom/svg_element.h"
#include "mitto/dom/svg_element_factory.h"
#include "mitto/dom/svg_elements.h"
#include "mitto/dom/text.h"
#include "mitto/dom/tree_walker.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "mitto/encoding/text_codec.h"
#include "mitto/encoding/text_encoding_registry.h"
#include "mitto/encoding/url_with_encoding.h"
#include "third_party/icu/source/i18n/unicode/gregocal.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_canon.h"
#include "url/url_util.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"

namespace mitto {

namespace {

// Whether `node` is an element of the SVG namespace whose local name is
// `local_name`, case-sensitively.
bool IsSVGElementNamed(const Node* node, std::u16string_view local_name) {
  if (!node || !node->IsElementNode()) {
    return false;
  }
  const QualifiedName& name = static_cast<const Element*>(node)->TagQName();
  return name.namespace_uri == html_names::kSvgNamespace &&
         name.local_name == local_name;
}

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

// How deep write() calls may nest, as Blink's kCMaxWriteRecursionDepth.
constexpr int kMaxWriteRecursionDepth = 21;

// Blink's NestingLevelIncrementer.
class NestingLevelIncrementer {
 public:
  explicit NestingLevelIncrementer(int& level) : level_(level) { ++*level_; }
  NestingLevelIncrementer(const NestingLevelIncrementer&) = delete;
  NestingLevelIncrementer& operator=(const NestingLevelIncrementer&) = delete;
  ~NestingLevelIncrementer() { --*level_; }

 private:
  const raw_ref<int> level_;
};

// The window of the entered context, as Blink's EnteredDOMWindow() gives it.
Window* EnteredWindow() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate->InContext()) {
    return nullptr;
  }
  return bindings::V8Window::ToWrappable(
      isolate, isolate->GetEnteredOrMicrotaskContext()->Global());
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
  // new Document(): the origin of the current window's document. A context
  // that is no window's, as the XML fuzz target's, whose fragments make
  // template documents, gives none.
  Document* const document = bindings::MakeGarbageCollected<Document>(false);
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate || !isolate->InContext()) {
    return document;
  }
  const Window* const window = bindings::V8Window::ToWrappable(
      isolate, isolate->GetCurrentContext()->Global());
  if (window && window->HasDocument()) {
    document->SetOrigin(window->GetDocument().GetOrigin());
  }
  return document;
}

// static
Document* Document::CreateHTML() {
  return HTMLDocument::Create();
}

Document::Document(bool is_html)
    : Node(nullptr),
      is_html_(is_html),
      content_type_(is_html ? u"text/html" : u"application/xml") {
  // An HTMLDocument clears its XML version, as Blink's does.
  if (!is_html) {
    xml_version_ = u"1.0";
  }
  SetDocumentToSelf(this);
}

Document::~Document() = default;

const bindings::WrapperTypeInfo* Document::GetWrapperTypeInfo() const {
  return &bindings::V8Document::kWrapperTypeInfo;
}

void Document::Trace(cppgc::Visitor* visitor) const {
  Node::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(css_target_);
  visitor->Trace(images_);
  visitor->Trace(embeds_);
  visitor->Trace(links_);
  visitor->Trace(forms_);
  visitor->Trace(scripts_);
  visitor->Trace(anchors_);
  visitor->Trace(applets_);
  visitor->Trace(all_);
  visitor->Trace(feature_policy_);
  visitor->Trace(script_runner_);
  visitor->Trace(fetcher_);
  current_script_stack_.Trace(visitor);
  pending_render_blocking_scripts_.Trace(visitor);
  visitor->Trace(template_document_);
  visitor->Trace(template_document_host_);
  visitor->Trace(implementation_);
  visitor->Trace(traversals_);
  visitor->Trace(parser_);
  visitor->Trace(load_timing_);
  popover_auto_stack_.Trace(visitor);
  popover_hint_stack_.Trace(visitor);
  visitor->Trace(popover_hint_stack_parent_);
  visitor->Trace(popover_removal_root_);
}

void Document::SetPopoverHintStackParent(HTMLElement* parent) {
  popover_hint_stack_parent_ = parent;
}

mitto::Location* Document::Location() const {
  return window_ ? window_->Location() : nullptr;
}

WindowTasks* Document::GetTasks() const {
  if (window_) {
    return window_->GetTasks();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!isolate || !isolate->InContext()) {
    return nullptr;
  }
  return Window::Current().GetTasks();
}

url::Origin Document::GetOrigin() const {
  return origin_ ? *origin_ : url::Origin::Create(GURL(url_));
}

// SecurityOrigin::Domain(): the host, which an opaque origin lacks.
std::u16string Document::Domain() const {
  const url::Origin origin = GetOrigin();
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
SVGSVGElement* Document::RootElement() const {
  Element* const element = DocumentElement();
  if (!element || element->NamespaceURI() != html_names::kSvgNamespace ||
      element->LocalName() != u"svg") {
    return nullptr;
  }
  return static_cast<SVGSVGElement*>(element);
}

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
  if (DocumentLoadTiming* const timing = load_timing_.Get()) {
    switch (readiness) {
      case Readiness::kLoading:
        if (timing->dom_loading.is_null()) {
          timing->dom_loading = base::TimeTicks::Now();
        }
        break;
      case Readiness::kInteractive:
        if (timing->dom_interactive.is_null()) {
          timing->dom_interactive = base::TimeTicks::Now();
          // Chrome's renderer tells its browser process, which answers with
          // the navigation's confidence from a later task: high, as for any
          // navigation once the browser has started, with its noise.
          if (WindowTasks* const tasks =
                  window_ ? window_->GetTasks() : nullptr) {
            tasks->PostTask(
                TaskType::kInternalNavigationAssociated,
                base::BindOnce(
                    [](const cppgc::Persistent<DocumentLoadTiming>& timing) {
                      timing->high_confidence =
                          PerformanceTimingConfidence::DrawHighConfidence();
                    },
                    cppgc::Persistent<DocumentLoadTiming>(timing)));
          }
        }
        break;
      case Readiness::kComplete:
        if (timing->dom_complete.is_null()) {
          timing->dom_complete = base::TimeTicks::Now();
        }
        break;
    }
  }
  readiness_ = readiness;
  bindings::EventInit init;
  Event* const event = Event::Create(u"readystatechange", init);
  event->SetTrusted(true);
  DispatchEvent(event);
}

bool Document::PageDismissalEventBeingDispatched() const {
  switch (load_event_progress_) {
    case LoadEventProgress::kBeforeUnloadEventInProgress:
    case LoadEventProgress::kPageHideInProgress:
    case LoadEventProgress::kUnloadVisibilityChangeInProgress:
    case LoadEventProgress::kUnloadEventInProgress:
      return true;
    default:
      return false;
  }
}

void Document::SetInitialEmptyDocument() {
  initial_empty_ = true;
  load_event_progress_ = LoadEventProgress::kLoadEventCompleted;
}

bool Document::DispatchBeforeUnloadEvent() {
  // The HTML Standard's prompt to unload, which FrameLoader::ShouldClose()
  // runs with the ignore-opens-during-unload counter up, so that open() and
  // write() do nothing meanwhile.
  const IgnoreOpensDuringUnloadCountIncrementer ignore_opens_during_unload(
      *this);
  if (!window_ || !Body()) {
    return true;
  }
  // A navigation that a beforeunload listener starts.
  if (load_event_progress_ == LoadEventProgress::kBeforeUnloadEventInProgress) {
    return false;
  }
  BeforeUnloadEvent* const event = BeforeUnloadEvent::Create();
  {
    const base::AutoReset<LoadEventProgress> in_progress(
        &load_event_progress_, LoadEventProgress::kBeforeUnloadEventInProgress);
    window_->DispatchEventWithTarget(event, this);
  }
  if (!event->ReturnValue().empty() || event->DefaultPrevented()) {
    // Intervention::GenerateReport(), as no user ever activated the page.
    window_->AddConsoleMessage(
        ConsoleLevel::kError,
        "Blocked attempt to show a 'beforeunload' confirmation panel for a "
        "frame that never had a user gesture since its load. "
        "https://www.chromestatus.com/feature/5082396709879808");
  }
  // FrameLoader::BeforeUnloadDoneWillUnload().
  load_event_progress_ = LoadEventProgress::kBeforeUnloadEventHandled;
  return true;
}

HttpRefreshScheduler& Document::GetHttpRefreshScheduler() {
  if (!http_refresh_scheduler_) {
    http_refresh_scheduler_ = std::make_unique<HttpRefreshScheduler>(*this);
  }
  return *http_refresh_scheduler_;
}

void Document::Abort() {
  // CancelParsing(): the parser goes, and the document is complete. The
  // refresh that the document scheduled goes with it.
  CancelParsing();
}

void Document::DispatchUnloadEvents() {
  // The HTML Standard's unload a document, which Blink's frame runs with the
  // ignore-opens-during-unload counter up, so that open() and write() do
  // nothing meanwhile.
  const IgnoreOpensDuringUnloadCountIncrementer ignore_opens_during_unload(
      *this);
  // The parser stops for good, and what more of the body comes goes unread.
  DetachParser();
  if (load_event_progress_ == LoadEventProgress::kLoadEventNotRun ||
      load_event_progress_ >= LoadEventProgress::kPageHideInProgress ||
      !window_) {
    return;
  }
  load_event_progress_ = LoadEventProgress::kPageHideInProgress;
  window_->DispatchEventWithTarget(
      PageTransitionEvent::CreateTrusted(u"pagehide", /*persisted=*/false),
      this);
  // The page is visible until this changes it, which Hidden() reads.
  const bool page_visible = window_ && !Hidden();
  load_event_progress_ = LoadEventProgress::kUnloadVisibilityChangeInProgress;
  if (page_visible) {
    for (const char16_t* type :
         {u"visibilitychange", u"webkitvisibilitychange"}) {
      bindings::EventInit init;
      init.bubbles = true;
      Event* const event = Event::Create(type, init);
      event->SetTrusted(true);
      DispatchEvent(event);
    }
  }
  load_event_progress_ = LoadEventProgress::kUnloadEventInProgress;
  if (window_) {
    bindings::EventInit init;
    Event* const event = Event::Create(u"unload", init);
    event->SetTrusted(true);
    unload_event_start_ = base::TimeTicks::Now();
    window_->DispatchEventWithTarget(event, this);
    unload_event_end_ = base::TimeTicks::Now();
  }
  load_event_progress_ = LoadEventProgress::kUnloadEventHandled;
}

std::u16string Document::CompatMode() const {
  return InQuirksMode() ? u"BackCompat" : u"CSS1Compat";
}

DocumentType* Document::Doctype() const {
  for (Node* child = FirstChild(); child; child = child->NextSibling()) {
    if (child->IsDocumentTypeNode()) {
      return static_cast<DocumentType*>(child);
    }
  }
  return nullptr;
}

std::u16string Document::Dir() const {
  Element* const root = DocumentElement();
  if (!root || !root->HasHTMLTagName(u"html")) {
    return u"";
  }
  return static_cast<HTMLElement*>(root)->Dir();
}

void Document::SetDir(const std::u16string& dir) {
  Element* const root = DocumentElement();
  if (root && root->HasHTMLTagName(u"html")) {
    root->SetAttributeWithoutValidation(u"dir", dir);
  }
}

std::u16string Document::BodyAttribute(std::u16string_view name) const {
  const HTMLElement* const body = Body();
  const std::u16string* const value =
      body ? body->FindAttribute(name) : nullptr;
  return value ? *value : u"";
}

void Document::SetBodyAttribute(std::u16string_view name,
                                const std::u16string& value) {
  HTMLElement* const body = Body();
  if (!body || (IsHTMLDocument() && body->HasHTMLTagName(u"frameset"))) {
    return;
  }
  const std::u16string* const current = body->FindAttribute(name);
  if (current && *current == value) {
    return;
  }
  body->SetAttributeWithoutValidation(name, value);
}

std::u16string Document::Title() const {
  // The HTML Standard: in a document whose element is an SVG svg element, the
  // first SVG title element child of it.
  if (Element* const root = DocumentElement(); IsSVGElementNamed(root, u"svg")) {
    for (const Node* child = root->FirstChild(); child;
         child = child->NextSibling()) {
      if (IsSVGElementNamed(child, u"title")) {
        return StripAndCollapseWhitespace(child->ChildTextContent());
      }
    }
    return std::u16string();
  }
  // Otherwise the first HTML title element in the document, in tree order.
  for (const Node* node = FirstChild(); node; node = node->NextInTree(this)) {
    if (node->IsElementNode() &&
        static_cast<const Element*>(node)->HasHTMLTagName(u"title")) {
      return StripAndCollapseWhitespace(node->ChildTextContent());
    }
  }
  return std::u16string();
}

void Document::SetXmlVersion(const std::optional<std::u16string>& version,
                             bindings::ExceptionState& exception_state) {
  if (version != u"1.0") {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        base::StrCat({"This document does not support the XML version '",
                      base::UTF16ToUTF8(version.value_or(u"")), "'."}));
    return;
  }
  xml_version_ = version;
}

void Document::SetXmlStandalone(bool standalone,
                                bindings::ExceptionState& exception_state) {
  xml_standalone_ = standalone ? StandaloneStatus::kStandalone
                               : StandaloneStatus::kNotStandalone;
}

std::u16string Document::CharacterSet() const {
  return base::ASCIIToUTF16(encoding_);
}

void Document::SetEncoding(std::string_view encoding,
                           bool detected_heuristically,
                           bool saw_decoding_error) {
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
  saw_decoding_error_ = saw_decoding_error;
}

void Document::SetTitle(const std::u16string& title) {
  Element* const root = DocumentElement();
  if (IsSVGElementNamed(root, u"svg")) {
    // The root's first SVG title element child, or a new one inserted as its
    // first child.
    Element* title_element = nullptr;
    for (Node* child = root->FirstChild(); child; child = child->NextSibling()) {
      if (IsSVGElementNamed(child, u"title")) {
        title_element = static_cast<Element*>(child);
        break;
      }
    }
    if (!title_element) {
      title_element =
          CreateElement(html_names::kSvgNamespace, std::nullopt, u"title");
      if (Node* const first = root->FirstChild()) {
        root->ParserInsertBefore(title_element, *first);
      } else {
        root->ParserAppendChild(title_element);
      }
    }
    // String replace all, one record for observers.
    const ChildListMutationScope mutation(*title_element);
    title_element->RemoveChildren();
    if (!title.empty()) {
      title_element->ParserAppendChild(Text::Create(*this, title));
    }
    return;
  }
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

void Document::SetBody(HTMLElement* body,
                       bindings::ExceptionState& exception_state) {
  if (!body) {
    exception_state.ThrowDOMException(
        "HierarchyRequestError",
        "The 1st argument provided is either null, or an invalid HTMLElement "
        "object.");
    return;
  }
  Element* const root = DocumentElement();
  if (!root) {
    exception_state.ThrowDOMException("HierarchyRequestError",
                                      "No document element exists.");
    return;
  }
  if (!body->HasHTMLTagName(u"body") && !body->HasHTMLTagName(u"frameset")) {
    exception_state.ThrowDOMException(
        "HierarchyRequestError",
        base::StrCat({"The new body element is of type '",
                      base::UTF16ToUTF8(body->TagName()),
                      "'. It must be either a 'BODY' or 'FRAMESET' element."}));
    return;
  }
  HTMLElement* const old_body = Body();
  if (old_body == body) {
    return;
  }
  if (old_body) {
    root->ReplaceChild(body, old_body, exception_state);
  } else {
    root->AppendChild(body, exception_state);
  }
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

Event* Document::CreateEvent(const std::u16string& event_type,
                             bindings::ExceptionState& exception_state) {
  Event* const event = CreateEventOfInterfaceName(event_type);
  // Blink's check for crbug.com/392584: a TouchEvent only where touch events
  // are exposed.
  if (event &&
      (!base::EqualsCaseInsensitiveASCII(event_type, u"TouchEvent") ||
       bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
           ->features()
           .IsEnabled(bindings::RuntimeFeature::kTouchEventFeatureDetection))) {
    return event;
  }
  exception_state.ThrowDOMException(
      "NotSupportedError",
      base::StrCat({"The provided event type ('", base::UTF16ToUTF8(event_type),
                    "') is invalid."}));
  return nullptr;
}

ProcessingInstruction* Document::CreateProcessingInstruction(
    const std::u16string& target,
    const std::u16string& data,
    bindings::ExceptionState& exception_state) {
  if (!ProcessingInstruction::IsValidName(target)) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        base::StrCat({"The target provided ('", base::UTF16ToUTF8(target),
                      "') is not a valid name."}));
    return nullptr;
  }
  if (data.find(u"?>") != std::u16string::npos) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        base::StrCat({"The data provided ('", base::UTF16ToUTF8(data),
                      "') contains '?>'."}));
    return nullptr;
  }
  return ProcessingInstruction::Create(*this, target, data);
}

CDATASection* Document::CreateCDATASection(
    const std::u16string& data,
    bindings::ExceptionState& exception_state) {
  if (IsHTMLDocument()) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        "This operation is not supported for HTML documents.");
    return nullptr;
  }
  if (data.find(u"]]>") != std::u16string::npos) {
    exception_state.ThrowDOMException(
        "InvalidCharacterError",
        "String cannot contain ']]>' since that is the end delimiter of a "
        "CData section.");
    return nullptr;
  }
  return CDATASection::Create(*this, data);
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
  // The DOM Standard's createElement(): an XHTML document's elements are of
  // the HTML namespace too, with their names as given.
  if (content_type_ == u"application/xhtml+xml") {
    return CreateHTMLElement(*this, local_name);
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

namespace {

// TreeScope::FindAnchorWithName(): the element of the ID, or else the first
// link of the name, matched exactly, and in quirks mode in any ASCII case if
// none matches exactly.
Element* FindAnchorWithName(const Document& document,
                            const std::u16string& name) {
  if (name.empty()) {
    return nullptr;
  }
  if (Element* const element = document.GetElementById(name)) {
    return element;
  }
  for (Node* node = document.FirstChild(); node;
       node = node->NextInTree(&document)) {
    if (!node->IsElementNode() ||
        !static_cast<Element*>(node)->HasHTMLTagName(u"a")) {
      continue;
    }
    auto* const anchor = static_cast<Element*>(node);
    const std::u16string* const anchor_name = anchor->FindAttribute(u"name");
    if (!anchor_name) {
      continue;
    }
    if (*anchor_name == name ||
        (document.InQuirksMode() &&
         base::EqualsCaseInsensitiveASCII(*anchor_name, name))) {
      return anchor;
    }
  }
  return nullptr;
}

// TreeScope::FindAnchor(): the indicated part of the document, for the
// fragment of its URL as it is and percent-decoded as UTF-8, or the document
// itself for "" and "top".
Node* FindAnchor(Document& document, std::string_view fragment) {
  if (Element* const element =
          FindAnchorWithName(document, base::UTF8ToUTF16(fragment))) {
    return element;
  }
  url::RawCanonOutputW<1024> decoded;
  url::DecodeUrlEscapeSequences(fragment, url::DecodeUrlMode::kUtf8, &decoded);
  const std::u16string name(decoded.view());
  if (Element* const element = FindAnchorWithName(document, name)) {
    return element;
  }
  if (fragment.empty() || base::EqualsCaseInsensitiveASCII(name, u"top")) {
    return &document;
  }
  return nullptr;
}

// DisplayLockUtilities::RevealAutoExpandableAncestors(), the HTML Standard's
// ancestor revealing algorithm. From the indicated node up, an ancestor whose
// hidden attribute is "until-found" gets a bubbling beforematch event and
// then loses the attribute, and a closed details element opens if the way up
// goes through its contents, innermost first. It stops at the first ancestor
// that is no longer connected, or that a listener has already revealed or
// changed. A Page has no shadow trees, so the node tree stands in for Blink's
// flat tree, and a details element's contents are the children that Blink's
// ManuallyAssignSlots() puts in its content slot: all but its first summary.
void RevealAutoExpandableAncestors(Node& target) {
  enum class AncestorType {
    kDetails,
    kHiddenUntilFound,
  };
  std::vector<std::pair<cppgc::Persistent<Element>, AncestorType>>
      ancestors_to_reveal;
  for (Node* ancestor = &target; ancestor; ancestor = ancestor->ParentNode()) {
    if (ancestor->IsElementNode()) {
      auto* const element = static_cast<Element*>(ancestor);
      const std::u16string* const hidden = element->FindAttribute(u"hidden");
      if (element->IsHTMLElement() && hidden &&
          base::EqualsCaseInsensitiveASCII(*hidden, u"until-found")) {
        ancestors_to_reveal.emplace_back(element,
                                         AncestorType::kHiddenUntilFound);
      }
    }
    Node* const parent = ancestor->ParentNode();
    if (!parent || !parent->IsElementNode() ||
        !static_cast<Element*>(parent)->HasHTMLTagName(u"details")) {
      continue;
    }
    auto* const details = static_cast<HTMLDetailsElement*>(parent);
    const bool in_content_slot =
        (ancestor->IsElementNode() || ancestor->IsTextNode()) &&
        ancestor != details->MainSummary();
    if (!details->IsOpen() && in_content_slot) {
      ancestors_to_reveal.emplace_back(details, AncestorType::kDetails);
    }
  }
  for (const auto& [element, type] : ancestors_to_reveal) {
    if (!element->IsConnected()) {
      return;
    }
    if (type == AncestorType::kHiddenUntilFound) {
      const std::u16string* hidden = element->FindAttribute(u"hidden");
      if (!hidden ||
          !base::EqualsCaseInsensitiveASCII(*hidden, u"until-found")) {
        return;
      }
      bindings::EventInit init;
      init.bubbles = true;
      Event* const event = Event::Create(u"beforematch", init);
      event->SetTrusted(true);
      element->DispatchEvent(event);
      hidden = element->FindAttribute(u"hidden");
      if (!element->IsConnected() || !hidden ||
          !base::EqualsCaseInsensitiveASCII(*hidden, u"until-found")) {
        return;
      }
      element->RemoveAttribute(u"hidden");
    } else {
      auto* const details = static_cast<HTMLDetailsElement*>(element.Get());
      if (details->IsOpen()) {
        return;
      }
      details->SetAttributeWithoutValidation(u"open", std::u16string());
    }
  }
}

}  // namespace

void Document::ProcessUrlFragment(const GURL& url) {
  // With no fragment there is nothing to indicate, but a target that a
  // fragment indicated before goes.
  if (!url.has_ref() && !css_target_) {
    return;
  }
  // RemoveFragmentDirectives(): what follows ":~:" directs the browser.
  std::string_view fragment = url.ref();
  if (const size_t directives = fragment.find(":~:");
      directives != std::string_view::npos) {
    fragment = fragment.substr(0, directives);
  }
  Node* const anchor = FindAnchor(*this, fragment);
  css_target_ = anchor && anchor->IsElementNode()
                    ? static_cast<Element*>(anchor)
                    : nullptr;
  // ElementFragmentAnchor::TryCreate() reveals the indicated part where the
  // fragment may scroll to it: on every navigation but one whose scroll
  // offset comes back from history, which a Page, without layout, never
  // restores.
  if (anchor) {
    RevealAutoExpandableAncestors(*anchor);
  }
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

void Document::AddNamedItem(const std::u16string& name) {
  if (name.empty()) {
    return;
  }
  if (named_item_counts_[name]++ == 0) {
    named_items_.push_back(name);
  }
  // As each time an element adds it, which gives the property again if a
  // page deleted it.
  NamedItemAdded(*this, name);
}

void Document::RemoveNamedItem(const std::u16string& name) {
  if (name.empty()) {
    return;
  }
  // Blink's HashCountedSet::erase() does nothing for a name it does not
  // hold: an img counts its ID once when it gets the ID in the tree but twice
  // when it comes in with both, and takes it away twice as it leaves.
  if (const auto found = named_item_counts_.find(name);
      found != named_item_counts_.end() && --found->second == 0) {
    named_item_counts_.erase(found);
    std::erase(named_items_, name);
  }
  NamedItemRemoved(*this, name);
}

bool Document::HasNamedItem(const std::u16string& name) const {
  return named_item_counts_.contains(name);
}

void Document::AddElementById(const std::u16string& id) {
  ++id_counts_[id];
}

void Document::RemoveElementById(const std::u16string& id) {
  if (const auto found = id_counts_.find(id);
      found != id_counts_.end() && --found->second == 0) {
    id_counts_.erase(found);
  }
}

bool Document::HasElementWithId(const std::u16string& id) const {
  return id_counts_.contains(id);
}

bool Document::ContainsMultipleElementsWithId(const std::u16string& id) const {
  const auto found = id_counts_.find(id);
  return found != id_counts_.end() && found->second > 1;
}

HTMLCollection* Document::WindowNamedItems(const std::u16string& name) {
  return EnsureNodeLists().NamedItems(
      *this, HTMLCollection::Type::kWindowNamedItems, name);
}

HTMLCollection* Document::DocumentNamedItems(const std::u16string& name) {
  return EnsureNodeLists().NamedItems(
      *this, HTMLCollection::Type::kDocumentNamedItems, name);
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
  saw_elements_in_known_namespaces_ = true;
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
  Document* const copy = CloneDocumentWithoutChildren();
  copy->SetCompatibilityMode(compat_mode_);
  // Blink's CloneDataFromDocument() copies the encoding and the MIME type too.
  copy->encoding_ = encoding_;
  copy->content_type_ = content_type_;
  copy->encoding_detected_heuristically_ = encoding_detected_heuristically_;
  return copy;
}

Document* Document::CloneDocumentWithoutChildren() const {
  return is_html_ ? CreateHTML() : Create();
}

DOMImplementation* Document::Implementation() {
  if (!implementation_) {
    implementation_ = DOMImplementation::Create(*this);
  }
  return implementation_;
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

std::string Document::UnresolvedUrlSpec(std::u16string_view url) const {
  return ::mitto::UnresolvedUrlSpec(base_url_, url, encoding_);
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
  // data or javascript. Blink also gives it to a document of no execution
  // context, but every document of a Page has one, those that a script makes
  // as much as its window's, as DocumentInit::WithExecutionContext() gives
  // them theirs.
  base_element_url_ = !data_or_javascript ? base_element_url : FallbackBaseUrl();
  UpdateBaseUrl();
}

void Document::UpdateBaseUrl() {
  base_url_ =
      !base_element_url_.is_empty() ? base_element_url_ : FallbackBaseUrl();
  if (!base_url_.is_valid()) {
    base_url_ = GURL();
  }
}

void Document::ParseAndSetReferrerPolicy(std::u16string_view policy,
                                         ReferrerPolicySource source) {
  FetchReferrerPolicy referrer_policy = FetchReferrerPolicy::kDefault;
  bool policy_is_valid = false;
  if (source == ReferrerPolicySource::kHttpHeader) {
    // SecurityPolicy::ReferrerPolicyFromHeaderValue(): the last token that
    // names a policy, and none at all if a token is not letters and hyphens.
    policy_is_valid = true;
    for (const std::u16string_view token : base::SplitStringPiece(
             policy, u",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
      const FetchReferrerPolicy parsed = FetchReferrerPolicyFromString(token);
      if (parsed != FetchReferrerPolicy::kDefault) {
        referrer_policy = parsed;
      } else if (!std::ranges::all_of(token, [](char16_t c) {
                   return base::IsAsciiAlpha(c) || c == '-';
                 })) {
        policy_is_valid = false;
        break;
      }
    }
    policy_is_valid =
        policy_is_valid && referrer_policy != FetchReferrerPolicy::kDefault;
  } else {
    referrer_policy = FetchReferrerPolicyFromStringWithLegacyKeywords(policy);
    policy_is_valid = referrer_policy != FetchReferrerPolicy::kDefault;
  }
  if (policy_is_valid) {
    referrer_policy_ = referrer_policy;
    return;
  }
  // A header that names no policy leaves the document's, with no message.
  if (source == ReferrerPolicySource::kHttpHeader || !window_) {
    return;
  }
  const std::string reason =
      policy.find(',') != std::u16string_view::npos
          ? "A policy specified by a meta element must contain only one token."
          : base::StrCat(
                {"The value '", base::UTF16ToUTF8(policy),
                 "' is not one of 'always', 'default', 'never', "
                 "'origin-when-crossorigin', 'no-referrer', "
                 "'no-referrer-when-downgrade', 'origin', "
                 "'origin-when-cross-origin', 'same-origin', 'strict-origin', "
                 "'strict-origin-when-cross-origin', or 'unsafe-url'."});
  window_->AddConsoleMessage(
      ConsoleLevel::kError,
      base::StrCat({"Failed to set referrer policy: ", reason,
                    " The referrer policy has been left unchanged."}));
}

ResourceFetcher& Document::Fetcher() {
  if (!fetcher_) {
    fetcher_ = bindings::MakeGarbageCollected<ResourceFetcher>(*this);
  }
  return *fetcher_;
}

DocumentScriptRunner& Document::GetScriptRunner() {
  if (!script_runner_) {
    script_runner_ =
        bindings::MakeGarbageCollected<DocumentScriptRunner>(*this);
  }
  return *script_runner_;
}

bindings::ScriptWrappable* Document::CurrentScriptForBinding() const {
  return current_script_stack_.empty() ? nullptr : current_script_stack_.back();
}

void Document::PushCurrentScript(Element* element) {
  current_script_stack_.push_back(element);
}

void Document::PopCurrentScript(Element* element) {
  CHECK_EQ(current_script_stack_.back(), element);
  current_script_stack_.pop_back();
}

void Document::DecrementLoadEventDelayCount() {
  CHECK_GT(load_event_delay_count_, 0);
  if (--load_event_delay_count_ > 0 || load_event_check_pending_) {
    return;
  }
  // CheckLoadEventSoon(): a timer of no delay on the DOM manipulation task
  // source.
  WindowTasks* const tasks = window_ ? window_->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  load_event_check_pending_ = true;
  tasks->PostTask(TaskType::kDOMManipulation,
                  base::BindOnce(
                      [](const cppgc::WeakPersistent<Document>& document) {
                        if (!document) {
                          return;
                        }
                        document->load_event_check_pending_ = false;
                        document->CheckCompleted();
                      },
                      cppgc::WeakPersistent<Document>(this)));
}

void Document::DecrementLoadEventDelayCountAndCheckLoadEvent() {
  CHECK_GT(load_event_delay_count_, 0);
  if (--load_event_delay_count_ == 0) {
    CheckCompleted();
  }
}

void Document::SetLoadTiming(DocumentLoadTiming* timing) {
  load_timing_ = timing;
}

DocumentParser& Document::OpenForNavigation(DocumentParser::Client& client,
                                            DocumentParser::Kind kind) {
  return ImplicitOpen(&client, kind);
}

DocumentParser& Document::ImplicitOpen(DocumentParser::Client* client,
                                       DocumentParser::Kind kind) {
  RemoveChildren();
  SetCompatibilityMode(CompatibilityMode::kNoQuirks);
  DetachParser();
  // Document::CreateParser(): a document that is not HTML has the XML
  // parser.
  if (!is_html_) {
    kind = DocumentParser::Kind::kXml;
  }
  parser_ = bindings::MakeGarbageCollected<DocumentParser>(*this, client, kind);
  // The parser may run scripts before this returns, so it is held here.
  const cppgc::Persistent<DocumentParser> parser = parser_.Get();
  parsing_state_ = ParsingState::kParsing;
  SetReadiness(Readiness::kLoading);
  if (load_event_progress_ != LoadEventProgress::kLoadEventInProgress &&
      !PageDismissalEventBeingDispatched()) {
    load_event_progress_ = LoadEventProgress::kLoadEventNotRun;
  }
  return *parser;
}

MarkupPosition Document::EventHandlerPosition() const {
  return parser_ ? parser_->TextPosition() : MarkupPosition();
}

void Document::DetachParser() {
  if (DocumentParser* const parser = parser_.Release()) {
    parser->Detach();
  }
}

void Document::CancelParsing() {
  // An open() or write() right after a navigation starts does nothing, as
  // Firefox has it and Blink copies, so as not to cancel the navigation.
  if (parser_ && parser_->IsParsing()) {
    ignore_opens_and_writes_for_abort_ = true;
  }
  DetachParser();
  parsing_state_ = ParsingState::kFinishedParsing;
  SetReadiness(Readiness::kComplete);
  // A document that stops before its body has no more to wait for.
  if (window_) {
    window_->BeginLifecycleUpdates();
  }
  if (http_refresh_scheduler_) {
    http_refresh_scheduler_->Cancel();
  }
  if (load_event_progress_ < LoadEventProgress::kLoadEventCompleted) {
    load_event_progress_ = LoadEventProgress::kLoadEventCompleted;
  }
}

void Document::FinishedParsing() {
  parsing_state_ = ParsingState::kInDOMContentLoaded;
  // DOMContentLoaded at the document, from the task that ends the parse, as
  // Blink dispatches it, which bubbles to the window.
  DocumentLoadTiming* const timing = load_timing_.Get();
  if (timing && timing->dom_content_loaded_event_start.is_null()) {
    timing->dom_content_loaded_event_start = base::TimeTicks::Now();
  }
  bindings::EventInit init;
  init.bubbles = true;
  Event* const event = Event::Create(u"DOMContentLoaded", init);
  event->SetTrusted(true);
  DispatchEvent(event);
  if (timing && timing->dom_content_loaded_event_end.is_null()) {
    timing->dom_content_loaded_event_end = base::TimeTicks::Now();
  }
  parsing_state_ = ParsingState::kFinishedParsing;
  v8::MicrotasksScope::PerformCheckpoint(v8::Isolate::GetCurrent());
  // FrameLoader::FinishedParsing(), for a document of a window: one that
  // never had a body renders now, the fragment of its URL indicates its
  // target, then it may complete.
  if (window_) {
    BeginLifecycleUpdatesIfRenderingReady();
    ProcessUrlFragment(GURL(UrlForBinding()));
    CheckCompleted();
  }
}

void Document::WillInsertBody() {
  // The fetches that wait for the body start.
  if (window_) {
    Fetcher().LoosenLoadThrottlingPolicy();
  }
  BeginLifecycleUpdatesIfRenderingReady();
}

void Document::AddPendingRenderBlockingScript(Element& script) {
  // Blink makes the manager for an HTML document of a window, which takes
  // scripts until the body is there.
  if (!window_ || !is_html_ || Body()) {
    return;
  }
  pending_render_blocking_scripts_.push_back(&script);
}

void Document::RemovePendingRenderBlockingScript(Element& script) {
  bool found = false;
  pending_render_blocking_scripts_.EraseIf([&script, &found](Element* item) {
    found |= item == &script;
    // One that the collector took has gone too, as from Blink's set.
    return item == &script || !item;
  });
  // Document::RenderBlockingResourceUnblocked().
  if (found && Body()) {
    BeginLifecycleUpdatesIfRenderingReady();
  }
}

bool Document::AddPendingRenderBlockingSheet(Element& owner) {
  if (!window_ || !is_html_ || Body()) {
    return false;
  }
  pending_render_blocking_scripts_.push_back(&owner);
  return true;
}

void Document::AddPendingBlockingSheet(Element& owner, PendingSheetType type) {
  const bool is_render_blocking = AddPendingRenderBlockingSheet(owner);
  if (type != PendingSheetType::kBlocking) {
    return;
  }
  ++pending_script_blocking_stylesheets_;
  // One in the body blocks the parser, which needs it for the style of what
  // follows, as HTMLDocumentParser::DidAddPendingParserBlockingStylesheet()
  // waits.
  if (!is_render_blocking) {
    ++pending_parser_blocking_stylesheets_;
    if (DocumentParser* const parser = parser_.Get()) {
      parser->DidAddPendingParserBlockingStylesheet();
    }
  }
}

void Document::RemovePendingBlockingSheet(Element& owner,
                                          PendingSheetType type) {
  bool was_render_blocking = false;
  pending_render_blocking_scripts_.EraseIf(
      [&owner, &was_render_blocking](Element* item) {
        if (item != &owner || was_render_blocking) {
          return !item;
        }
        was_render_blocking = true;
        return true;
      });
  if (was_render_blocking && Body()) {
    BeginLifecycleUpdatesIfRenderingReady();
  }
  if (type != PendingSheetType::kBlocking) {
    return;
  }
  if (!was_render_blocking && pending_parser_blocking_stylesheets_ > 0 &&
      --pending_parser_blocking_stylesheets_ == 0) {
    if (DocumentParser* const parser = parser_.Get()) {
      parser->DidLoadAllPendingParserBlockingStylesheets();
    }
  }
  CHECK_GT(pending_script_blocking_stylesheets_, 0);
  if (--pending_script_blocking_stylesheets_ > 0) {
    return;
  }
  // DidLoadAllScriptBlockingResources(): the scripts that wait for style
  // sheets run from a task of the networking task source.
  WindowTasks* const tasks = window_ ? window_->GetTasks() : nullptr;
  if (!tasks) {
    return;
  }
  tasks->PostTask(
      TaskType::kNetworking,
      base::BindOnce(
          [](const cppgc::WeakPersistent<Document>& document) {
            if (!document || !document->IsScriptExecutionReady()) {
              return;
            }
            if (DocumentParser* const parser = document->parser_.Get()) {
              parser->ExecuteScriptsWaitingForResources();
            }
          },
          cppgc::WeakPersistent<Document>(this)));
}

void Document::BeginLifecycleUpdatesIfRenderingReady() {
  if (!window_) {
    return;
  }
  pending_render_blocking_scripts_.EraseIf([](Element* item) { return !item; });
  if (pending_render_blocking_scripts_.empty()) {
    window_->BeginLifecycleUpdates();
  }
}

void Document::CheckCompleted() {
  // Document::ShouldComplete(), with what a Page's documents hold back: the
  // async and in-order scripts.
  if (parsing_state_ != ParsingState::kFinishedParsing ||
      IsDelayingLoadEvent() ||
      load_event_progress_ == LoadEventProgress::kLoadEventInProgress) {
    return;
  }
  SetReadiness(Readiness::kComplete);
  if (load_event_progress_ == LoadEventProgress::kLoadEventNotRun) {
    DispatchLoadEventAndFinalize();
  }
  // The refresh's delay starts once the load event has finished.
  if (window_ && http_refresh_scheduler_) {
    http_refresh_scheduler_->MaybeStartTimer();
  }
}

void Document::DispatchLoadEventAndFinalize() {
  load_event_progress_ = LoadEventProgress::kLoadEventInProgress;
  // The parser goes, in case the load event's listeners write.
  DetachParser();
  if (run_before_load_event_) {
    std::move(run_before_load_event_).Run();
  }
  if (Window* const window = window_.Get()) {
    // LocalDOMWindow::DispatchLoadEvent(): load at the window, with the
    // document as its target, its times marked the first time.
    bindings::EventInit init;
    Event* const event = Event::Create(u"load", init);
    event->SetTrusted(true);
    DocumentLoadTiming* const timing = load_timing_.Get();
    if (timing && timing->load_event_start.is_null()) {
      timing->load_event_start = base::TimeTicks::Now();
      window->DispatchEventWithTarget(event, this);
      timing->load_event_end = base::TimeTicks::Now();
    } else {
      window->DispatchEventWithTarget(event, this);
    }
    // LocalDOMWindow::DispatchLoadAndPageshowEvents(): pageshow right after
    // load, at the window with the document as its target.
    window->DispatchEventWithTarget(
        PageTransitionEvent::CreateTrusted(u"pageshow", /*persisted=*/false),
        this);
  }
  load_event_progress_ = LoadEventProgress::kLoadEventCompleted;
  if (window_ && !did_dispatch_load_event_.is_null()) {
    did_dispatch_load_event_.Run();
  }
}

void Document::RemoveAllEventListenersRecursively() {
  for (Node* node = this; node; node = node->NextInTree(this)) {
    node->RemoveAllEventListeners();
  }
  if (window_) {
    window_->RemoveAllEventListeners();
  }
}

Document* Document::Open(const std::u16string& type,
                         const std::u16string& replace,
                         bindings::ExceptionState& exception_state) {
  // The type is ignored, and "replace" only counted, as in Blink.
  Open(EnteredWindow(), exception_state);
  return this;
}

Window* Document::Open(const std::u16string& url,
                       const std::u16string& name,
                       const std::u16string& features,
                       bindings::ExceptionState& exception_state) {
  if (!window_) {
    exception_state.ThrowDOMException("InvalidAccessError",
                                      "The document has no window associated.");
    return nullptr;
  }
  return window_->Open(url, name, features, exception_state);
}

void Document::Open(Window* entered_window,
                    bindings::ExceptionState& exception_state) {
  if (!is_html_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "Only HTML documents support open().");
    return;
  }
  if (throw_on_dynamic_markup_insertion_count_) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "Custom Element constructor should not use open().");
    return;
  }
  // A Page's documents all have its one window's origin, so the check that
  // the entered window's is the same always passes.
  if (entered_window && !entered_window->HasContext()) {
    return;
  }
  // A parser that runs a script it reached keeps the document.
  if (parser_ && parser_->IsParsing() && parser_->IsExecutingScript()) {
    return;
  }
  if (ignore_opens_during_unload_count_ || ignore_opens_and_writes_for_abort_) {
    return;
  }
  // A document of a window takes the entered window's URL, without its
  // fragment if that is another window.
  if (window_ && entered_window && entered_window->Document()) {
    GURL url(entered_window->Document()->UrlForBinding());
    if (entered_window != window_ && url.has_ref()) {
      GURL::Replacements replacements;
      replacements.ClearRef();
      url = url.ReplaceComponents(replacements);
    }
    const std::u16string spec = base::UTF8ToUTF16(url.possibly_invalid_spec());
    if (spec != url_) {
      SetUrl(spec);
    }
  }
  Open();
}

void Document::Open() {
  // A navigation that a document started and that waits for its response,
  // or a refresh due now, stops, with the document's loading, as
  // FrameLoader::StopAllLoaders() has it, unless an unload event is being
  // dispatched. Canceling javascript:
  // URLs and form submissions, and the unload of child frames, come with the
  // forms and frames that a page cannot have yet.
  NavigationClient* const navigation =
      window_ ? window_->GetNavigationClient() : nullptr;
  if (navigation &&
      (navigation->HasProvisionalNavigation() ||
       (http_refresh_scheduler_ &&
        http_refresh_scheduler_->IsScheduledWithin(base::TimeDelta()))) &&
      !PageDismissalEventBeingDispatched()) {
    CancelParsing();
    navigation->StopAllLoaders();
  }
  // Every listener of the document's nodes and its window goes, then a
  // script-created parser loads the document anew.
  RemoveAllEventListenersRecursively();
  ImplicitOpen(nullptr);
}

void Document::Close(bindings::ExceptionState& exception_state) {
  if (!is_html_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "Only HTML documents support close().");
    return;
  }
  if (throw_on_dynamic_markup_insertion_count_) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "Custom Element constructor should not use close().");
    return;
  }
  Close();
}

void Document::Close() {
  // Nothing without a script-created parser that is parsing.
  if (!parser_ || !parser_->WasCreatedByScript() || !parser_->IsParsing()) {
    return;
  }
  // The explicit end of the parser's input.
  const cppgc::Persistent<DocumentParser> parser = parser_.Get();
  parser->Finish();
  if (!parser_ || !parser_->IsParsing()) {
    SetReadiness(Readiness::kComplete);
  }
  CheckCompleted();
}

void Document::SetContent(const std::u16string& content) {
  // Only once the document is ready for it.
  if (parser_ && parser_->IsParsing() && parser_->IsExecutingScript()) {
    return;
  }
  if (ignore_opens_during_unload_count_) {
    return;
  }
  Open();
  if (const cppgc::Persistent<DocumentParser> parser = parser_.Get()) {
    parser->Insert(content);
  }
  Close();
}

void Document::Write(const std::vector<std::u16string>& text,
                     bindings::ExceptionState& exception_state) {
  // The strings are one, as a TrustedHTML is its string with no policy to
  // check it against.
  std::u16string string;
  for (const std::u16string& piece : text) {
    string += piece;
  }
  Write(string, EnteredWindow(), exception_state);
}

void Document::Writeln(const std::vector<std::u16string>& text,
                       bindings::ExceptionState& exception_state) {
  std::u16string string;
  for (const std::u16string& piece : text) {
    string += piece;
  }
  string += u'\n';
  Write(string, EnteredWindow(), exception_state);
}

void Document::Write(const std::u16string& text,
                     Window* entered_window,
                     bindings::ExceptionState& exception_state) {
  if (!is_html_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "Only HTML documents support write().");
    return;
  }
  if (throw_on_dynamic_markup_insertion_count_) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "Custom Element constructor should not use write().");
    return;
  }
  if (entered_window && !entered_window->HasContext()) {
    return;
  }
  if (ignore_opens_and_writes_for_abort_) {
    return;
  }
  // Writes nested deeper than Blink's 21 do nothing, nor do the writes
  // within them once one has been too deep.
  const NestingLevelIncrementer nesting_level(write_recursion_depth_);
  write_recursion_is_too_deep_ =
      (write_recursion_depth_ > 1 && write_recursion_is_too_deep_) ||
      write_recursion_depth_ > kMaxWriteRecursionDepth;
  if (write_recursion_is_too_deep_) {
    return;
  }
  if (!parser_ || !parser_->HasInsertionPoint()) {
    if (ignore_destructive_write_count_) {
      if (window_) {
        window_->AddConsoleMessage(
            ConsoleLevel::kWarning,
            "Failed to execute 'write' on 'Document': It isn't possible to "
            "write into a document from an asynchronously-loaded external "
            "script unless it is explicitly opened.");
      }
      return;
    }
    if (ignore_opens_during_unload_count_) {
      return;
    }
    Open(entered_window, exception_state);
    if (exception_state.HadException()) {
      return;
    }
  }
  if (!parser_) {
    return;
  }
  const cppgc::Persistent<DocumentParser> parser = parser_.Get();
  parser->Insert(text);
}

}  // namespace mitto
