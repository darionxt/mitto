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

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "mitto/dom/html_collection.h"
#include "mitto/dom/node.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class Attr;
class Comment;
class CookieJar;
class DocumentFragment;
class CustomElementRegistry;
class DocumentType;
class Element;
class HTMLAllCollection;
class HTMLElement;
class HTMLHeadElement;
class Location;
class PermissionsPolicy;
class Text;
class Window;

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
  std::u16string CharacterSet() const { return u"UTF-8"; }
  std::u16string ContentType() const;
  DocumentType* Doctype() const;
  Element* DocumentElement() const { return FirstElementChild(); }
  std::u16string Title() const;
  void SetTitle(const std::u16string& title);
  HTMLElement* Body() const;
  HTMLHeadElement* Head() const;
  Window* DefaultView() const { return window_.Get(); }
  // The window's location, and none for a document of no window.
  mitto::Location* Location() const;
  // The host of the document's origin, and nothing for an opaque one.
  std::u16string Domain() const;
  std::u16string DesignMode() const { return design_mode_ ? u"on" : u"off"; }
  void SetDesignMode(const std::u16string& value);
  Element* ScrollingElement() const;
  Element* ActiveElement() const;
  // What a page of a tab in front reads, which a document of no window is
  // not, as Blink's documents without a page say.
  bool Hidden() const { return !window_; }
  std::u16string VisibilityState() const {
    return window_ ? u"visible" : u"hidden";
  }
  std::u16string VisibilityStateAsString() const { return VisibilityState(); }
  // What a top-level page that permissions policy allows them reads, and a
  // document of no window does not. It is never in full screen or in
  // picture-in-picture.
  bool FullscreenEnabled() const { return window_; }
  bool PictureInPictureEnabled() const { return window_; }
  Element* FullscreenElement() const { return nullptr; }
  // An SVG document's root, which an HTML document lacks.
  bindings::ScriptWrappable* RootElement() const { return nullptr; }
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
  Attr* CreateAttribute(const std::u16string& local_name,
                        bindings::ExceptionState& exception_state);
  Attr* CreateAttributeNS(const std::optional<std::u16string>& namespace_uri,
                          const std::u16string& qualified_name,
                          bindings::ExceptionState& exception_state);
  Comment* CreateComment(const std::u16string& data);
  DocumentFragment* CreateDocumentFragment();
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
  Node* ImportNode(Node* node,
                   const bindings::BooleanOrImportNodeOptions& options,
                   bindings::ExceptionState& exception_state);
  Node* AdoptNode(Node* node, bindings::ExceptionState& exception_state);

  // Node's.
  uint16_t GetNodeType() const override;
  std::u16string NodeName() const override;

  // What the DOM and the parser use.
  bool IsHTMLDocument() const { return is_html_; }
  CompatibilityMode GetCompatibilityMode() const { return compat_mode_; }
  void SetCompatibilityMode(CompatibilityMode mode) { compat_mode_ = mode; }
  bool InQuirksMode() const {
    return compat_mode_ == CompatibilityMode::kQuirks;
  }
  // The window whose document this is, which defaultView gives.
  void SetWindow(Window* window) { window_ = window; }
  // The URL of the document, serialized, which URL and documentURI give.
  void SetUrl(std::u16string url) { url_ = std::move(url); }
  // The time of its response's Last-Modified, which lastModified gives.
  void SetLastModified(base::Time time) { last_modified_ = time; }
  // Where document.cookie reaches the cookies of the document's Session. A
  // document that has a window and no jar has an opaque origin, as about:blank
  // has in a new tab, and its cookie attribute throws.
  void SetCookieJar(CookieJar* cookie_jar) { cookie_jar_ = cookie_jar; }
  CookieJar* GetCookieJar() const { return cookie_jar_; }
  Readiness GetReadiness() const { return readiness_; }
  // Sets the readiness, and fires readystatechange at the document for
  // interactive and complete, as Blink's Document::SetReadyState() does.
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
  // `url` resolved against the document's base URL, as Blink's CompleteURL()
  // resolves it: the document's URL, since the Runtime does not read <base>
  // yet. An invalid GURL when it does not resolve.
  GURL CompleteURL(std::u16string_view url) const;
  // The document that owns the contents of this document's template
  // elements: a new document of the same kind with no window, made the first
  // time one asks, and the document itself for such a document, as Blink's
  // EnsureTemplateDocument().
  Document& EnsureTemplateDocument();

 protected:
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  void ThrowCookiesDenied(bindings::ExceptionState& exception_state) const;

  HTMLCollection* Collection(cppgc::Member<HTMLCollection>& cached,
                             HTMLCollection::Type type);

  const bool is_html_;
  bool design_mode_ = false;
  CompatibilityMode compat_mode_ = CompatibilityMode::kNoQuirks;
  std::u16string url_ = u"about:blank";
  std::optional<base::Time> last_modified_;
  // A document that no parser is loading is complete, as Blink's starts.
  Readiness readiness_ = Readiness::kComplete;
  // The Page program's, which outlives every document.
  raw_ptr<CookieJar> cookie_jar_ = nullptr;
  cppgc::Member<Window> window_;
  cppgc::Member<HTMLCollection> images_;
  cppgc::Member<HTMLCollection> embeds_;
  cppgc::Member<HTMLCollection> links_;
  cppgc::Member<HTMLCollection> forms_;
  cppgc::Member<HTMLCollection> scripts_;
  cppgc::Member<HTMLCollection> anchors_;
  cppgc::Member<HTMLCollection> applets_;
  cppgc::Member<HTMLAllCollection> all_;
  cppgc::Member<PermissionsPolicy> feature_policy_;
  cppgc::Member<Document> template_document_;
  // The document whose template document this is.
  cppgc::Member<Document> template_document_host_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_H_
