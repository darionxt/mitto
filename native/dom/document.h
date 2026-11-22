// The DOM's Document: the root of a tree, its doctype and element, and the
// factory of its nodes.
//
// A Page's window has one, about:blank's until its Session loads one, and a
// page can make more, which have no window. A document is an HTML document or
// not, as `new Document()` makes one, which decides how its elements' names
// read and which namespace createElement() puts an element in.
//
// Upstream: third_party/blink/renderer/core/dom/document.h
// Upstream: third_party/blink/renderer/core/dom/document.cc
// Upstream: third_party/blink/renderer/modules/storage_access/document_storage_access.cc

#ifndef MITTO_DOM_DOCUMENT_H_
#define MITTO_DOM_DOCUMENT_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/time/time.h"
#include "mitto/dom/document_parser.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/html_collection.h"
#include "mitto/dom/markup_position.h"
#include "mitto/dom/node.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class AttachedTraversals;
class Attr;
class CDATASection;
class Comment;
class CacheStorageHost;
class IndexedDbHost;
class StorageBucketHost;
class CookieJar;
class DOMImplementation;
class DocumentFragment;
class ResourceFetcher;
class CustomElementRegistry;
class DocumentLoadTiming;
class DocumentScriptRunner;
class DocumentType;
class Element;
class Event;
class HTMLAllCollection;
class HTMLElement;
class HTMLHeadElement;
class HTMLScriptElement;
class HttpRefreshScheduler;
class Location;
class NodeIterator;
class PermissionsPolicy;
class ProcessingInstruction;
class Range;
class SVGSVGElement;
class Text;
class TreeWalker;
class Window;
class WindowTasks;

class Document : public Node {
 public:
  // The document's mode, which its doctype decides as the HTML Standard says.
  enum class CompatibilityMode {
    kNoQuirks,
    kQuirks,
    kLimitedQuirks,
  };

  // The HTML Standard's current document readiness, which readyState gives.
  enum class Readiness {
    kLoading,
    kInteractive,
    kComplete,
  };

  // Blink's ParsingState: whether a parser loads the document, and whether
  // DOMContentLoaded is being dispatched once it has finished.
  enum class ParsingState {
    kParsing,
    kInDOMContentLoaded,
    kFinishedParsing,
  };

  // Blink's LoadEventProgress: how far the document is through its load event
  // and, once a navigation leaves it, through being unloaded.
  enum class LoadEventProgress {
    kLoadEventNotRun,
    kLoadEventInProgress,
    kLoadEventCompleted,
    kBeforeUnloadEventInProgress,
    kBeforeUnloadEventHandled,
    kPageHideInProgress,
    kUnloadVisibilityChangeInProgress,
    kUnloadEventInProgress,
    kUnloadEventHandled,
  };

  // new Document(): an XML document, which is not an HTML document.
  static Document* Create();
  // An HTML document at about:blank, an HTMLDocument.
  static Document* CreateHTML();

  explicit Document(bool is_html);
  ~Document() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string UrlForBinding() const { return url_; }
  std::u16string CompatMode() const;
  // The name of the document's encoding, which charset and inputEncoding give
  // too.
  std::u16string CharacterSet() const;
  std::u16string ContentType() const { return content_type_; }
  // xmlEncoding, xmlVersion and xmlStandalone, which the XML parser sets from
  // an XML declaration, as Blink's Document holds them: no encoding, and a
  // version that is null for an HTML document and "1.0" for any other. Only
  // "1.0" can be set, as Blink's XMLDocumentParser::SupportsXMLVersion()
  // says.
  const std::optional<std::u16string>& XmlEncoding() const {
    return xml_encoding_;
  }
  const std::optional<std::u16string>& XmlVersion() const {
    return xml_version_;
  }
  void SetXmlVersion(const std::optional<std::u16string>& version,
                     bindings::ExceptionState& exception_state);
  bool XmlStandalone() const {
    return xml_standalone_ == StandaloneStatus::kStandalone;
  }
  void SetXmlStandalone(bool standalone,
                        bindings::ExceptionState& exception_state);
  DocumentType* Doctype() const;
  Element* DocumentElement() const { return FirstElementChild(); }
  // SVG's rootElement: the document's element if it is an svg element of
  // SVG's namespace, as Blink's SVGDocumentExtensions::rootElement() has it.
  SVGSVGElement* RootElement() const;
  std::u16string Title() const;
  void SetTitle(const std::u16string& title);
  // The html element's dir, "" for a document element that is not one.
  std::u16string Dir() const;
  void SetDir(const std::u16string& dir);
  // The body's text, bgcolor, link, vlink and alink attributes, a frameset's
  // too, as Blink's legacy color attributes read and write them.
  std::u16string FgColor() const { return BodyAttribute(u"text"); }
  void SetFgColor(const std::u16string& color) {
    SetBodyAttribute(u"text", color);
  }
  std::u16string BgColor() const { return BodyAttribute(u"bgcolor"); }
  void SetBgColor(const std::u16string& color) {
    SetBodyAttribute(u"bgcolor", color);
  }
  std::u16string LinkColor() const { return BodyAttribute(u"link"); }
  void SetLinkColor(const std::u16string& color) {
    SetBodyAttribute(u"link", color);
  }
  std::u16string VlinkColor() const { return BodyAttribute(u"vlink"); }
  void SetVlinkColor(const std::u16string& color) {
    SetBodyAttribute(u"vlink", color);
  }
  std::u16string AlinkColor() const { return BodyAttribute(u"alink"); }
  void SetAlinkColor(const std::u16string& color) {
    SetBodyAttribute(u"alink", color);
  }
  HTMLElement* Body() const;
  // Blink's setBody(): a body or frameset element takes the place of the
  // body, or goes at the end of the document element, with its checks.
  void SetBody(HTMLElement* body, bindings::ExceptionState& exception_state);
  HTMLHeadElement* Head() const;
  Window* DefaultView() const { return window_.Get(); }
  // The tasks that the document's elements queue theirs in, as Blink's
  // GetTaskRunner(): its window's, or for a document of no window, such as
  // one that createHTMLDocument() made, the current window's, as such a
  // document of Blink shares the window's execution context. Null outside a
  // window's context.
  WindowTasks* GetTasks() const;
  // The window's location, and none for a document of no window.
  mitto::Location* Location() const;
  // The host of the document's origin, and nothing for an opaque one.
  std::u16string Domain() const;
  // The document's origin: the one that a document made by new Document() or
  // DOMImplementation takes from the document that made it, as Blink's
  // DocumentInit::WithExecutionContext() gives it, or else its URL's.
  url::Origin GetOrigin() const;
  void SetOrigin(const url::Origin& origin) { origin_ = origin; }
  std::u16string DesignMode() const { return design_mode_ ? u"on" : u"off"; }
  bool InDesignMode() const { return design_mode_; }
  void SetDesignMode(const std::u16string& value);
  Element* ScrollingElement() const;
  Element* ActiveElement() const;
  // What a page of a tab in front reads, which a document of no window is
  // not, as Blink's documents without a page say, nor one that a navigation
  // unloads, from its visibilitychange on, as Blink's IsPageVisible() says.
  bool Hidden() const {
    return !window_ || load_event_progress_ >=
                           LoadEventProgress::kUnloadVisibilityChangeInProgress;
  }
  std::u16string VisibilityState() const {
    return Hidden() ? u"hidden" : u"visible";
  }
  std::u16string VisibilityStateAsString() const { return VisibilityState(); }
  // What a top-level page that permissions policy allows them reads, and a
  // document of no window does not. It is never in full screen or in
  // picture-in-picture.
  bool FullscreenEnabled() const { return window_; }
  bool PictureInPictureEnabled() const { return window_; }
  Element* FullscreenElement() const { return nullptr; }
  // The window's custom elements, and none for a document of no window.
  mitto::CustomElementRegistry* CustomElementRegistry() const;
  // When the response says it last changed, or else now, in the local time of
  // the Page's time zone.
  std::u16string LastModified() const;
  // The document's collections, each the same object on every read, as Blink
  // caches them, plugins being embeds.
  HTMLCollection* Images();
  HTMLCollection* Embeds();
  HTMLCollection* Links();
  HTMLCollection* Forms();
  HTMLCollection* Scripts();
  HTMLCollection* Anchors();
  HTMLCollection* Applets();
  HTMLAllCollection* All();
  std::u16string Cookie(bindings::ExceptionState& exception_state) const;
  void SetCookie(const std::u16string& value,
                 bindings::ExceptionState& exception_state);
  std::u16string ReadyState() const;
  // DocumentStorageAccess: a top-level document has the access to its cookies
  // that a profile allows, which a new profile does, on a secure page, and
  // none elsewhere.
  v8::Local<v8::Promise> HasStorageAccess() const;
  // The same object each time, made the first time a page asks, and none for
  // a document without a window, as Blink has no execution context to make it
  // from.
  PermissionsPolicy* FeaturePolicy();
  // The document's own, the same object on every read.
  DOMImplementation* Implementation();
  Attr* CreateAttribute(const std::u16string& local_name,
                        bindings::ExceptionState& exception_state);
  Attr* CreateAttributeNS(const std::optional<std::u16string>& namespace_uri,
                          const std::u16string& qualified_name,
                          bindings::ExceptionState& exception_state);
  Comment* CreateComment(const std::u16string& data);
  // Blink's checks: a target that is an XML name and data without "?>".
  ProcessingInstruction* CreateProcessingInstruction(
      const std::u16string& target,
      const std::u16string& data,
      bindings::ExceptionState& exception_state);
  // Throws for an HTML document, and for data with "]]>".
  CDATASection* CreateCDATASection(const std::u16string& data,
                                   bindings::ExceptionState& exception_state);
  DocumentFragment* CreateDocumentFragment();
  // Blink's Document::createEvent(): an event of the interface that
  // `event_type` names, not initialized, from event_factory.h, or Chrome's
  // NotSupportedError for a name that no factory takes, and for TouchEvent
  // while touch events are not exposed.
  Event* CreateEvent(const std::u16string& event_type,
                     bindings::ExceptionState& exception_state);
  Element* CreateElementForBinding(const std::u16string& local_name,
                                   bindings::ExceptionState& exception_state);
  Element* CreateElementForBinding(
      const std::u16string& local_name,
      const bindings::DOMStringOrElementCreationOptions& options,
      bindings::ExceptionState& exception_state);
  Element* CreateElementNS(const std::optional<std::u16string>& namespace_uri,
                           const std::u16string& qualified_name,
                           bindings::ExceptionState& exception_state);
  Element* CreateElementNS(
      const std::optional<std::u16string>& namespace_uri,
      const std::u16string& qualified_name,
      const bindings::DOMStringOrElementCreationOptions& options,
      bindings::ExceptionState& exception_state);
  Text* CreateTextNode(const std::u16string& data);
  Element* GetElementById(const std::u16string& id) const;
  // The HTML Standard's dynamic markup insertion, as Blink's Document runs

  // HTMLDocument's named items, which the elements of an HTML document's tree
  // add and remove as Blink's AddNamedItem() and RemoveNamedItem() count them:
  // the names and IDs that document[name] finds. A window's document's
  // wrapper has a property of each, as Blink's LocalWindowProxy gives it one
  // when an element adds the name and takes it away when no element has it.
  void AddNamedItem(const std::u16string& name);
  void RemoveNamedItem(const std::u16string& name);
  bool HasNamedItem(const std::u16string& name) const;
  // The named items, each once, in the order in which they came.
  const std::vector<std::u16string>& NamedItems() const { return named_items_; }
  // The IDs of the elements in the document's tree, counted, as Blink's
  // TreeScope maps them, which the window's named properties object reads.
  void AddElementById(const std::u16string& id);
  void RemoveElementById(const std::u16string& id);
  bool HasElementWithId(const std::u16string& id) const;
  bool ContainsMultipleElementsWithId(const std::u16string& id) const;
  // The elements of `name` that the window's named properties object finds,
  // and that document[name] finds, each the same collection for the same
  // name, as Blink's WindowNamedItems() and DocumentNamedItems() cache them.
  HTMLCollection* WindowNamedItems(const std::u16string& name);
  HTMLCollection* DocumentNamedItems(const std::u16string& name);
  // it: open() with no more than two arguments opens the document anew, with
  // a script-created parser, and returns it, and with three opens a window,
  // as window.open() does. write() and writeln() insert markup at the
  // parser's insertion point, or open the document first if it has none.
  // close() ends the input of a script-created parser.
  Document* Open(const std::u16string& type,
                 const std::u16string& replace,
                 bindings::ExceptionState& exception_state);
  Window* Open(const std::u16string& url,
               const std::u16string& name,
               const std::u16string& features,
               bindings::ExceptionState& exception_state);
  void Close(bindings::ExceptionState& exception_state);
  void Write(const std::vector<std::u16string>& text,
             bindings::ExceptionState& exception_state);
  void Writeln(const std::vector<std::u16string>& text,
               bindings::ExceptionState& exception_state);
  // The script element whose classic script runs, and null outside one: an
  // HTMLOrSVGScriptElement, as the bindings return a union of interfaces.
  bindings::ScriptWrappable* CurrentScriptForBinding() const;
  Node* ImportNode(Node* node,
                   const bindings::BooleanOrImportNodeOptions& options,
                   bindings::ExceptionState& exception_state);
  Node* AdoptNode(Node* node, bindings::ExceptionState& exception_state);
  // The live list of the HTML elements whose name attribute is `name`.
  NodeList* GetElementsByName(const std::u16string& name);
  Range* CreateRange();
  NodeIterator* CreateNodeIterator(Node* root,
                                   uint32_t what_to_show,
                                   v8::Local<v8::Object> filter);
  TreeWalker* CreateTreeWalker(Node* root,
                               uint32_t what_to_show,
                               v8::Local<v8::Object> filter);

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

  // What the DOM and the parser use.
  bool IsHTMLDocument() const { return is_html_; }
  // Whether an XML declaration said standalone, as Blink's StandaloneStatus.
  enum class StandaloneStatus {
    kStandaloneUnspecified,
    kStandalone,
    kNotStandalone,
  };
  StandaloneStatus XmlStandaloneStatus() const { return xml_standalone_; }
  // Whether the XML parser read an XML declaration, which XMLSerializer
  // writes again for the document, and what it said.
  bool HasXMLDeclaration() const { return has_xml_declaration_; }
  void SetHasXMLDeclaration(bool has) { has_xml_declaration_ = has; }
  void SetXmlEncoding(const std::optional<std::u16string>& encoding) {
    xml_encoding_ = encoding;
  }
  // Whether the parser attaches the declarative shadow roots of its
  // templates, which a document that DOMParser makes does not, as Blink's
  // DeclarativeShadowRootAllowState.
  bool AllowsDeclarativeShadowRoots() const {
    return allow_declarative_shadow_roots_;
  }
  void SetAllowDeclarativeShadowRoots(bool allow) {
    allow_declarative_shadow_roots_ = allow;
  }
  // Blink's Document::SetContent(): the document opened, `content` parsed
  // with a script-created parser, and closed, as DOMParser fills an HTML
  // document.
  void SetContent(const std::u16string& content);
  CompatibilityMode GetCompatibilityMode() const { return compat_mode_; }
  // Sets the mode unless it is locked, as a text, JSON, image or media
  // document's no-quirks mode is.
  void SetCompatibilityMode(CompatibilityMode mode) {
    if (!compat_mode_locked_) {
      compat_mode_ = mode;
    }
  }
  void LockCompatibilityMode() { compat_mode_locked_ = true; }
  bool InQuirksMode() const {
    return compat_mode_ == CompatibilityMode::kQuirks;
  }
  // The window whose document this is, which defaultView gives.
  void SetWindow(Window* window) { window_ = window; }
  // The URL of the document, serialized, which URL and documentURI give.
  void SetUrl(std::u16string url);
  // The canonical name of the encoding that the document's body decodes with,
  // as text_encoding_registry.h has it: UTF-8 for a document that no body
  // has decoded into yet, as Blink's DocumentEncodingData starts.
  const std::string& Encoding() const { return encoding_; }
  bool EncodingWasDetectedHeuristically() const {
    return encoding_detected_heuristically_;
  }
  // Whether the decoder of the document's body met a byte that its encoding
  // cannot decode, which stops the decoder of XML.
  bool SawDecodingError() const { return saw_decoding_error_; }
  // Sets the encoding as the decoder of the document's body has it after each
  // piece, and whether it met an error, as Blink's Document::SetEncodingData()
  // does. When a <meta> found late changes windows-1252 to another, the text
  // of the document's <title>, decoded as windows-1252 already, decodes again
  // with the new one.
  void SetEncoding(std::string_view encoding,
                   bool detected_heuristically,
                   bool saw_decoding_error);
  // The time of its response's Last-Modified, which lastModified gives.
  void SetLastModified(base::Time time) { last_modified_ = time; }
  // The referrer of the navigation that loaded the document, which referrer
  // gives: none for one that the browser started, and for one that a document
  // started, what its request sent as Referer.
  const std::u16string& Referrer() const { return referrer_; }
  void SetReferrer(std::u16string referrer) { referrer_ = std::move(referrer); }
  // Where document.cookie reaches the cookies of the document's Session. A
  // document that has a window and no jar has an opaque origin, as about:blank
  // has in a new tab, and its cookie attribute throws.
  void SetCookieJar(CookieJar* cookie_jar) { cookie_jar_ = cookie_jar; }
  CookieJar* GetCookieJar() const { return cookie_jar_; }
  // Where the document's Cache API reaches its Session's caches, which only a
  // document that the Session loaded over HTTP or HTTPS has.
  void SetCacheStorageHost(CacheStorageHost* host) {
    cache_storage_host_ = host;
  }
  CacheStorageHost* GetCacheStorageHost() const { return cache_storage_host_; }
  // Where the document reaches its Session's IndexedDB, or null if its
  // origin may not.
  void SetIndexedDbHost(IndexedDbHost* host) { indexed_db_host_ = host; }
  IndexedDbHost* GetIndexedDbHost() const { return indexed_db_host_; }
  // Where the document's Storage Buckets API reaches its Session's buckets,
  // which only a document that the Session loaded over HTTP or HTTPS has.
  void SetStorageBucketHost(StorageBucketHost* host) {
    storage_bucket_host_ = host;
  }
  StorageBucketHost* GetStorageBucketHost() const {
    return storage_bucket_host_;
  }
  Readiness GetReadiness() const { return readiness_; }
  // Sets the readiness, marks when it changed the first time in the load
  // timing, and fires readystatechange at the document, as Blink's
  // Document::SetReadyState() does.
  void SetReadiness(Readiness readiness);
  // An element of the class that its namespace and name call for, as Blink's
  // Document::CreateRawElement() makes it: for the HTML namespace, the HTML
  // Standard's interface for the name, or HTMLElement while the Runtime lacks
  // that interface; for the SVG namespace, the interface of SVG for the name,
  // or SVGElement for a name it does not know; for MathML, MathMLElement;
  // and otherwise an Element. The tree builder and createElementNS() make
  // every element through it.
  Element* CreateElement(const std::optional<std::u16string>& namespace_uri,
                         const std::optional<std::u16string>& prefix,
                         const std::u16string& local_name);
  // The document that owns the contents of this document's template
  // elements: a new document of the same kind with no window, made the first
  // time one asks, and the document itself for such a document, as Blink's
  // EnsureTemplateDocument().
  Document& EnsureTemplateDocument();
  // Whether it is another document's template document.
  bool IsTemplateDocument() const { return template_document_host_; }

  // The URLs a document resolves, as Blink's Document has them.
  //
  // The HTML Standard's document base URL: the frozen base URL of the first
  // base element with an href in tree order, else the fallback base URL. A
  // base URL that is not valid is none, and about:blank for those who read
  // it.
  const GURL& BaseUrl() const;
  // Blink's Document::CompleteURL(): `url` resolved against the base URL,
  // with the query in the document's encoding, which every URL that the
  // document resolves goes through. An invalid GURL if it does not resolve.
  GURL CompleteUrl(std::u16string_view url) const;
  // The text that Blink's KURL keeps of a `url` that does not resolve, empty
  // for one that does.
  std::string UnresolvedUrlSpec(std::u16string_view url) const;
  // The document's URL, as no frame gives a document of a Page another.
  GURL FallbackBaseUrl() const { return GURL(url_); }
  // Blink's ValidBaseElementURL(): the URL that the first base element with
  // an href gives, if it is valid.
  GURL ValidBaseElementUrl() const {
    return base_element_url_.is_valid() ? base_element_url_ : GURL();
  }
  // Blink's Document::ProcessBaseElement(), once a base element comes, goes,
  // or changes its href.
  void ProcessBaseElement();
  // When the document was made, as Blink's Document::GetStartTime().
  base::TimeTicks StartTime() const { return start_time_; }

  // What the document's scripts use.
  //
  // The async and in-order scripts, which it makes the first time.
  DocumentScriptRunner& GetScriptRunner();
  // Where a referrer policy comes from, as Blink's ReferrerPolicySource.
  enum class ReferrerPolicySource {
    kHttpHeader,
    kMetaTag,
  };
  // The document's referrer policy, which a request that leaves its own to
  // the document goes with: kDefault, Chrome's, unless the response's
  // Referrer-Policy or a meta element named referrer set one.
  FetchReferrerPolicy GetReferrerPolicy() const { return referrer_policy_; }
  // ExecutionContext::ParseAndSetReferrerPolicy(): a header's last policy
  // that it names, or a meta element's one, with the legacy keywords, and
  // Chrome's console error for one that names none.
  void ParseAndSetReferrerPolicy(std::u16string_view policy,
                                 ReferrerPolicySource source);

  // What fetches the document's subresources, such as its images.
  ResourceFetcher& Fetcher();
  bool HasFetcher() const { return fetcher_.Get() != nullptr; }
  // The fetcher if the document has made one, as a const document reads it.
  ResourceFetcher* ExistingFetcher() const { return fetcher_.Get(); }
  // The HTML Standard's currentScript, as Blink keeps it for scripts that
  // run inside others.
  void PushCurrentScript(Element* element);
  void PopCurrentScript(Element* element);
  // Whether no style sheet blocks scripts.
  bool IsScriptExecutionReady() const {
    return pending_script_blocking_stylesheets_ == 0;
  }
  // Blink's PendingSheetType: what a style sheet that loads holds back.
  enum class PendingSheetType {
    kNone,
    // Nothing: one whose media does not match, or an alternate one.
    kNonBlocking,
    // Rendering, but not scripts: one that a script inserted.
    kDynamicRenderBlocking,
    // Scripts, and rendering before the body or the parser in it: one that
    // the parser inserted.
    kBlocking,
  };
  // StyleEngine::AddPendingBlockingSheet() and RemovePendingBlockingSheet()
  // of a sheet that `owner`, a link or a style element, loads: one of type
  // kBlocking blocks scripts, and before the body rendering, or in it the
  // parser. Removing the last that blocks scripts lets the scripts that wait
  // for them run, from a task, as Document::DidLoadAllScriptBlockingResources()
  // posts it.
  void AddPendingBlockingSheet(Element& owner, PendingSheetType type);
  void RemovePendingBlockingSheet(Element& owner, PendingSheetType type);
  // What holds the load event back, such as an async script that has not
  // run. Once nothing does, CheckCompleted() runs from a task, as Blink's
  // load event delay timer runs it.
  void IncrementLoadEventDelayCount() { ++load_event_delay_count_; }
  void DecrementLoadEventDelayCount();
  // The same, but checks the load event at once, from a task that a delay
  // held, such as an image's load event.
  void DecrementLoadEventDelayCountAndCheckLoadEvent();
  bool IsDelayingLoadEvent() const { return load_event_delay_count_ > 0; }

  // How the document loads, as Blink's Document and its frame's loader see
  // it.
  //
  // The parser that loads the document, its response's or the
  // script-created one of open(), until the document has loaded, and null
  // otherwise.
  DocumentParser* Parser() const { return parser_.Get(); }
  // ScriptController::EventHandlerPosition(): where the document's parser is,
  // while it has one, for the handlers that content attributes set, and the
  // start of the markup otherwise.
  MarkupPosition EventHandlerPosition() const;
  // Document::OpenForNavigation(): a parser for the document's response,
  // which `client` feeds, with the document loading.
  DocumentParser& OpenForNavigation(
      DocumentParser::Client& client,
      DocumentParser::Kind kind = DocumentParser::Kind::kHtml);
  // Document::CancelParsing(), as stopping the document's loading does: the
  // parser goes, the document is complete, and open() and write() do nothing
  // from then on if a parser was parsing.
  void CancelParsing();
  ParsingState GetParsingState() const { return parsing_state_; }
  // Document::FinishedParsing(), once the parser is done: DOMContentLoaded,
  // a microtask checkpoint, then CheckCompleted() for a document of a window.
  void FinishedParsing();
  // Document::WillInsertBody(), as the parser inserts the body: the fetches
  // that waited for it start, and a document with something to show begins
  // rendering, once no script blocks it.
  void WillInsertBody();
  // RenderBlockingResourceManager's pending scripts: a potentially
  // render-blocking script that is fetched before the body blocks the
  // rendering of a document of a window, as the HTML Standard's
  // render-blocking element set has it, until it runs or its fetch goes.
  // Removing the last one begins rendering if the body is there.
  void AddPendingRenderBlockingScript(Element& script);
  void RemovePendingRenderBlockingScript(Element& script);
  // RenderBlockingResourceManager::AddPendingStylesheet(): whether a style
  // sheet that `owner` loads blocks rendering, which one before the body
  // does, as a script does.
  bool AddPendingRenderBlockingSheet(Element& owner);
  // Document::CheckCompleted(): complete, and the window's load event, once
  // parsing has finished and nothing holds the load event back.
  void CheckCompleted();
  // The load timing of the window's navigation, which the readiness, the
  // parser's end and the load event mark, each the first time.
  void SetLoadTiming(DocumentLoadTiming* timing);
  // What runs each time the window's load event has been dispatched.
  void SetDidDispatchLoadEvent(base::RepeatingClosure did_dispatch) {
    did_dispatch_load_event_ = std::move(did_dispatch);
  }
  // What runs once, just before the window's next load event, as the XML
  // tree viewer's isolated world's onload does in Blink.
  void RunBeforeLoadEvent(base::OnceClosure closure) {
    run_before_load_event_ = std::move(closure);
  }
  // Whether the document has made an element of the HTML, SVG or MathML
  // namespace, as Blink's SawElementsInKnownNamespaces() says.
  bool SawElementsInKnownNamespaces() const {
    return saw_elements_in_known_namespaces_;
  }
  // Whether write() or writeln() runs.
  bool IsInDocumentWrite() const { return write_recursion_depth_ > 0; }

  // What a navigation does to the document, as Blink's Document has it.
  LoadEventProgress GetLoadEventProgress() const {
    return load_event_progress_;
  }
  void SetLoadEventProgress(LoadEventProgress progress) {
    load_event_progress_ = progress;
  }
  // Whether no frame under the document's still loads, which its load event
  // waits for, as Blink's AllDescendantsAreComplete() says.
  bool AllDescendantsAreComplete() const;
  bool LoadEventFinished() const {
    return load_event_progress_ >= LoadEventProgress::kLoadEventCompleted;
  }
  // Whether beforeunload, pagehide, the visibilitychange of unloading or
  // unload is being dispatched, while which no navigation starts.
  bool PageDismissalEventBeingDispatched() const;
  // Whether it is the Page's initial about:blank, whose load finished as it
  // was made, as Blink's initial empty document's does.
  bool IsInitialEmptyDocument() const { return initial_empty_; }
  void SetInitialEmptyDocument();
  // Document::DispatchBeforeUnloadEvent(): fires beforeunload at the window,
  // with the document as its target, if the document has a body. Returns
  // whether the navigation goes on, which it does even when the page cancels
  // the event: Chrome asks the user to stay only on a page that the user has
  // activated, which a page in a Page never is, and says so in the console.
  bool DispatchBeforeUnloadEvent();
  // Document::Abort(): CancelParsing(), so that nothing more of the body is
  // parsed, and the document is complete, with readystatechange, whose load
  // event then never fires, as a navigation that starts while it loads has
  // it.
  void Abort();
  // Document::DispatchUnloadEvents(): the parser stops for good, then
  // pagehide at the window, visibilitychange at the document while the page
  // is visible, and unload at the window, once a navigation's document is
  // about to take its place.
  void DispatchUnloadEvents();
  // When its unload event started and ended, null until it has.
  base::TimeTicks UnloadEventStart() const { return unload_event_start_; }
  base::TimeTicks UnloadEventEnd() const { return unload_event_end_; }
  // The refresh that the document's response or a meta element asks for,
  // made the first time.
  HttpRefreshScheduler& GetHttpRefreshScheduler();

  // The element that :target matches, which the fragment of the document's
  // URL indicates, as Blink's CSS target.
  Element* CssTarget() const { return css_target_.Get(); }
  // LocalFrameView::ProcessUrlFragment(), as ElementFragmentAnchor::TryCreate()
  // runs it: the CSS target becomes the element that the fragment of `url`
  // indicates, or none, once the document has finished parsing and after each
  // navigation within it. The indicated part is revealed: each closed details
  // element around it opens, and each ancestor hidden "until-found" gets
  // beforematch and shows.
  void ProcessUrlFragment(const GURL& url);

  // The language that the response's Content-Language and a content-language
  // meta give the document, as Blink's Document::SetContentLanguage() keeps
  // it: the last that was set, which removing the meta leaves.
  const std::optional<std::u16string>& ContentLanguage() const {
    return content_language_;
  }
  void SetContentLanguage(std::u16string language) {
    content_language_ = std::move(language);
  }

  // The live ranges and node iterators of the document, which the DOM tells
  // as its trees change, made when one is first attached.
  AttachedTraversals& Traversals();
  // Null while none was ever attached, so that a change costs nothing then.
  AttachedTraversals* ExistingTraversals() const { return traversals_.Get(); }

  // The open popovers of the auto and hint types, the topmost last, as
  // Blink's PopoverAutoStack() and PopoverHintStack() keep them, and the
  // popover that the hint stack opened from, if any.
  HeapVector<HTMLElement>& PopoverAutoStack() { return popover_auto_stack_; }
  HeapVector<HTMLElement>& PopoverHintStack() { return popover_hint_stack_; }
  HTMLElement* PopoverHintStackParent() const {
    return popover_hint_stack_parent_.Get();
  }
  void SetPopoverHintStackParent(HTMLElement* parent);
  // Whether a popover is being shown, and how many are being hidden, as
  // Blink's PopoverShowing() and PopoverHidingNestingCount() keep it.
  bool PopoverShowing() const { return popover_showing_; }
  void SetPopoverShowing(bool showing) { popover_showing_ = showing; }
  int PopoverHidingNestingCount() const {
    return popover_hiding_nesting_count_;
  }
  void IncrementPopoverHidingNestingCount() { ++popover_hiding_nesting_count_; }
  void DecrementPopoverHidingNestingCount() { --popover_hiding_nesting_count_; }
  // The root of a subtree that left the document while its popovers hide,
  // whose nodes count as connected meanwhile, as Blink's nodes are until
  // their own RemovedFrom().
  Node* PopoverRemovalRoot() const { return popover_removal_root_.Get(); }
  // Whether the document's response delivered a Content Security Policy in a
  // header, enforced or report-only, as Blink's HasHeaderDeliveredPolicy()
  // says; a Page enforces none of it yet.
  bool HasHeaderDeliveredContentSecurityPolicy() const {
    return has_header_delivered_content_security_policy_;
  }
  void SetHasHeaderDeliveredContentSecurityPolicy(bool value) {
    has_header_delivered_content_security_policy_ = value;
  }
  // Whether moveBefore() is moving a node, whose removal and insertion keep
  // its state, as Blink's StatePreservingAtomicMoveInProgress() says.
  bool StatePreservingAtomicMoveInProgress() const {
    return state_preserving_atomic_move_in_progress_;
  }
  void SetStatePreservingAtomicMoveInProgress(bool value) {
    state_preserving_atomic_move_in_progress_ = value;
  }
  void SetPopoverRemovalRoot(Node* root) { popover_removal_root_ = root; }
  // The MIME type that contentType gives, which an XML document and the loader
  // of a response set as Blink's SetMimeType() does.
  void SetContentType(std::u16string content_type) {
    content_type_ = std::move(content_type);
  }

 protected:
  Node* CloneWithoutChildren(Document& document) const override;
  // A new document of the same class, which a copy starts from, as Blink's
  // CloneDocumentWithoutChildren() makes it.
  virtual Document* CloneDocumentWithoutChildren() const;
 private:
  std::u16string BodyAttribute(std::u16string_view name) const;
  // Blink's SetBodyAttribute(): nothing for a frameset, no body, or the value
  // it already has.
  void SetBodyAttribute(std::u16string_view name, const std::u16string& value);
  friend class IgnoreDestructiveWriteCountIncrementer;
  friend class IgnoreOpensDuringUnloadCountIncrementer;
  friend class ThrowOnDynamicMarkupInsertionCountIncrementer;

  void ThrowCookiesDenied(bindings::ExceptionState& exception_state) const;
  // The document-open steps, from an `entered_window` that may be null, and
  // what they do once the checks have passed, as Blink splits them.
  void Open(Window* entered_window, bindings::ExceptionState& exception_state);
  void Open();
  // close() once its checks have passed: the end of a script-created parser's
  // input.
  void Close();
  // Document::ImplicitOpen(): the document emptied and loading, with a new
  // parser.
  DocumentParser& ImplicitOpen(
      DocumentParser::Client* client,
      DocumentParser::Kind kind = DocumentParser::Kind::kHtml);
  void DetachParser();
  // Document::BeginLifecycleUpdatesIfRenderingReady(): the window begins
  // rendering once no script blocks it.
  void BeginLifecycleUpdatesIfRenderingReady();
  // write(), once the strings are one.
  void Write(const std::u16string& text,
             Window* entered_window,
             bindings::ExceptionState& exception_state);
  // Document::DispatchLoadEventAndFinalize().
  void DispatchLoadEventAndFinalize();
  // Node::RemoveAllEventListenersRecursively() and the window's, as
  // document.open() erases them.
  void RemoveAllEventListenersRecursively();
  // Blink's Document::UpdateBaseURL().
  void UpdateBaseUrl();

  HTMLCollection* Collection(cppgc::Member<HTMLCollection>& cached,
                             HTMLCollection::Type type);

  const bool is_html_;
  // The origin it took from the document that made it, if any.
  std::optional<url::Origin> origin_;
  std::u16string content_type_;
  std::optional<std::u16string> xml_encoding_;
  std::optional<std::u16string> xml_version_;
  StandaloneStatus xml_standalone_ = StandaloneStatus::kStandaloneUnspecified;
  bool has_xml_declaration_ = false;
  bool allow_declarative_shadow_roots_ = true;
  bool saw_elements_in_known_namespaces_ = false;
  base::OnceClosure run_before_load_event_;
  // How many times each named item was added and not yet removed, and the
  // named items in the order in which they came.
  std::map<std::u16string, int> named_item_counts_;
  std::vector<std::u16string> named_items_;
  // How many elements of the tree have each ID.
  std::map<std::u16string, int> id_counts_;
  const base::TimeTicks start_time_ = base::TimeTicks::Now();
  bool design_mode_ = false;
  CompatibilityMode compat_mode_ = CompatibilityMode::kNoQuirks;
  bool compat_mode_locked_ = false;
  std::u16string url_ = u"about:blank";
  std::string encoding_ = "UTF-8";
  bool encoding_detected_heuristically_ = false;
  bool saw_decoding_error_ = false;
  std::optional<base::Time> last_modified_;
  // A document that no parser is loading is complete, as Blink's starts.
  Readiness readiness_ = Readiness::kComplete;
  std::u16string referrer_;
  bool initial_empty_ = false;
  std::unique_ptr<HttpRefreshScheduler> http_refresh_scheduler_;
  std::optional<std::u16string> content_language_;
  base::TimeTicks unload_event_start_;
  base::TimeTicks unload_event_end_;
  // The Page program's, which outlives every document.
  raw_ptr<CookieJar> cookie_jar_ = nullptr;
  raw_ptr<CacheStorageHost> cache_storage_host_ = nullptr;
  raw_ptr<IndexedDbHost> indexed_db_host_ = nullptr;
  raw_ptr<StorageBucketHost> storage_bucket_host_ = nullptr;
  cppgc::Member<Window> window_;
  cppgc::Member<Element> css_target_;
  cppgc::Member<HTMLCollection> images_;
  cppgc::Member<HTMLCollection> embeds_;
  cppgc::Member<HTMLCollection> links_;
  cppgc::Member<HTMLCollection> forms_;
  cppgc::Member<HTMLCollection> scripts_;
  cppgc::Member<HTMLCollection> anchors_;
  cppgc::Member<HTMLCollection> applets_;
  cppgc::Member<HTMLAllCollection> all_;
  cppgc::Member<PermissionsPolicy> feature_policy_;
  cppgc::Member<DOMImplementation> implementation_;
  cppgc::Member<AttachedTraversals> traversals_;
  cppgc::Member<Document> template_document_;
  // The document whose template document this is.
  cppgc::Member<Document> template_document_host_;
  // The frozen base URL of the first base element with an href, empty for
  // none, and the base URL that it gives, empty for one that is not valid.
  GURL base_element_url_;
  GURL base_url_ = GURL("about:blank");
  cppgc::Member<DocumentScriptRunner> script_runner_;
  cppgc::Member<ResourceFetcher> fetcher_;
  FetchReferrerPolicy referrer_policy_ = FetchReferrerPolicy::kDefault;
  HeapVector<Element> current_script_stack_;
  WeakHeapVector<Element> pending_render_blocking_scripts_;
  int load_event_delay_count_ = 0;
  int pending_script_blocking_stylesheets_ = 0;
  int pending_parser_blocking_stylesheets_ = 0;
  bool load_event_check_pending_ = false;
  cppgc::Member<DocumentParser> parser_;
  cppgc::Member<DocumentLoadTiming> load_timing_;
  base::RepeatingClosure did_dispatch_load_event_;
  // A document that no parser loads has finished parsing and loading, as
  // Blink's starts.
  ParsingState parsing_state_ = ParsingState::kFinishedParsing;
  LoadEventProgress load_event_progress_ =
      LoadEventProgress::kLoadEventCompleted;
  // The HTML Standard's counters of dynamic markup insertion.
  int ignore_destructive_write_count_ = 0;
  int ignore_opens_during_unload_count_ = 0;
  int throw_on_dynamic_markup_insertion_count_ = 0;
  // How deep write() calls nest, past which they do nothing, as Blink's
  // kCMaxWriteRecursionDepth has it.
  int write_recursion_depth_ = 0;
  bool write_recursion_is_too_deep_ = false;
  // The HTML Standard's active parser was aborted.
  bool ignore_opens_and_writes_for_abort_ = false;
  HeapVector<HTMLElement> popover_auto_stack_;
  HeapVector<HTMLElement> popover_hint_stack_;
  cppgc::Member<HTMLElement> popover_hint_stack_parent_;
  bool popover_showing_ = false;
  int popover_hiding_nesting_count_ = 0;
  cppgc::Member<Node> popover_removal_root_;
  bool state_preserving_atomic_move_in_progress_ = false;
  bool has_header_delivered_content_security_policy_ = false;
};

// The HTML Standard's ignore-destructive-writes counter of `document`, which
// may be null, is one higher while the scope lasts, as a script from a file
// runs.
class IgnoreDestructiveWriteCountIncrementer {
 public:
  explicit IgnoreDestructiveWriteCountIncrementer(Document* document)
      : count_(document ? &document->ignore_destructive_write_count_
                        : nullptr) {
    if (count_) {
      ++*count_;
    }
  }
  IgnoreDestructiveWriteCountIncrementer(
      const IgnoreDestructiveWriteCountIncrementer&) = delete;
  IgnoreDestructiveWriteCountIncrementer& operator=(
      const IgnoreDestructiveWriteCountIncrementer&) = delete;
  ~IgnoreDestructiveWriteCountIncrementer() {
    if (count_) {
      --*count_;
    }
  }

 private:
  const raw_ptr<int> count_;
};

// The ignore-opens-during-unload counter, as the unload of a document runs.
class IgnoreOpensDuringUnloadCountIncrementer {
 public:
  explicit IgnoreOpensDuringUnloadCountIncrementer(Document& document)
      : count_(document.ignore_opens_during_unload_count_) {
    ++*count_;
  }
  IgnoreOpensDuringUnloadCountIncrementer(
      const IgnoreOpensDuringUnloadCountIncrementer&) = delete;
  IgnoreOpensDuringUnloadCountIncrementer& operator=(
      const IgnoreOpensDuringUnloadCountIncrementer&) = delete;
  ~IgnoreOpensDuringUnloadCountIncrementer() { --*count_; }

 private:
  const raw_ref<int> count_;
};

// The throw-on-dynamic-markup-insertion counter, as the parser runs a custom
// element's constructor.
class ThrowOnDynamicMarkupInsertionCountIncrementer {
 public:
  explicit ThrowOnDynamicMarkupInsertionCountIncrementer(Document& document)
      : count_(document.throw_on_dynamic_markup_insertion_count_) {
    ++*count_;
  }
  ThrowOnDynamicMarkupInsertionCountIncrementer(
      const ThrowOnDynamicMarkupInsertionCountIncrementer&) = delete;
  ThrowOnDynamicMarkupInsertionCountIncrementer& operator=(
      const ThrowOnDynamicMarkupInsertionCountIncrementer&) = delete;
  ~ThrowOnDynamicMarkupInsertionCountIncrementer() { --*count_; }

 private:
  const raw_ref<int> count_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_H_
