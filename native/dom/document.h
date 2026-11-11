// The DOM's Document: the root of a tree, its doctype and element, and the
// factory of its nodes.
//
// A Page's window has one, about:blank's until a document loads (#183), and a
// page can make more, which have no window. A document is an HTML document or
// not, as `new Document()` makes one, which decides how its elements' names
// read and which namespace createElement() puts an element in.
//
// Upstream: third_party/blink/renderer/core/dom/document.h
// Upstream: third_party/blink/renderer/core/dom/document.cc

#ifndef MITTO_DOM_DOCUMENT_H_
#define MITTO_DOM_DOCUMENT_H_

#include <optional>
#include <string>

#include "mitto/dom/node.h"
#include "v8/include/cppgc/member.h"

namespace mitto {

class Comment;
class DocumentType;
class Element;
class HTMLElement;
class HTMLHeadElement;
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

  // new Document(): an XML document, which is not an HTML document.
  static Document* Create();
  // An HTML document at about:blank, an HTMLDocument.
  static Document* CreateHTML();

  explicit Document(bool is_html);
  ~Document() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string UrlForBinding() const { return u"about:blank"; }
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
  // A document of no browsing context has no location. The window's has one,
  // which the Runtime lacks until a Page loads documents (#183).
  bindings::ScriptWrappable* Location() const { return nullptr; }
  Comment* CreateComment(const std::u16string& data);
  Element* CreateElementForBinding(const std::u16string& local_name,
                                   bindings::ExceptionState& exception_state);
  Element* CreateElementForBinding(
      const std::u16string& local_name,
      const bindings::DOMStringOrElementCreationOptions& options,
      bindings::ExceptionState& exception_state);
  Text* CreateTextNode(const std::u16string& data);
  Element* GetElementById(const std::u16string& id) const;

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
  // An element of the class that its namespace and name call for, as Blink's
  // factories make it: for the HTML namespace, the HTML Standard's interface
  // for the name, or HTMLElement while the Runtime lacks that interface.
  Element* CreateElement(const std::optional<std::u16string>& namespace_uri,
                         const std::optional<std::u16string>& prefix,
                         const std::u16string& local_name);

 protected:
  Node* CloneWithoutChildren(Document& document) const override;

 private:
  const bool is_html_;
  CompatibilityMode compat_mode_ = CompatibilityMode::kNoQuirks;
  cppgc::Member<Window> window_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_H_
