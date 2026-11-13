// The DOM's Node: a node of a document's tree, with its parent, children and
// siblings, and the DOM standard's algorithms that change a tree.
//
// It follows Blink's Node and ContainerNode, whose checks and messages a page
// sees: a node holds its tree with members on V8's C++ heap, as Blink's do on
// Oilpan, so a node and its wrapper live and die together, and a node that a
// page no longer reaches goes when nothing in a tree holds it. The members of
// ParentNode, ChildNode and NonDocumentTypeChildNode, which the IDL gives to
// Document, Element, CharacterData and DocumentType, are here, as Blink has
// most of them in Node.
//
// Upstream: third_party/blink/renderer/core/dom/node.h
// Upstream: third_party/blink/renderer/core/dom/node.cc
// Upstream: third_party/blink/renderer/core/dom/container_node.h
// Upstream: third_party/blink/renderer/core/dom/container_node.cc

#ifndef MITTO_DOM_NODE_H_
#define MITTO_DOM_NODE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "mitto/bindings/dictionaries.h"
#include "mitto/dom/event_target.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class Document;
class Element;
class Event;
class EventDispatchHandlingState;
class HTMLCollection;
class Node;
class NodeList;
class NodeMutationObserverData;

// Nodes that C++ holds between allocations. A vector's buffer is not on the
// C++ heap, where the collector would see its members, so each is persistent.
using NodeVector = std::vector<cppgc::Persistent<Node>>;

class Node : public EventTarget {
 public:
  enum NodeType : uint16_t {
    kElementNode = 1,
    kAttributeNode = 2,
    kTextNode = 3,
    kCdataSectionNode = 4,
    kProcessingInstructionNode = 7,
    kCommentNode = 8,
    kDocumentNode = 9,
    kDocumentTypeNode = 10,
    kDocumentFragmentNode = 11,
  };

  // The values of compareDocumentPosition().
  enum DocumentPosition : uint16_t {
    kDocumentPositionDisconnected = 0x01,
    kDocumentPositionPreceding = 0x02,
    kDocumentPositionFollowing = 0x04,
    kDocumentPositionContains = 0x08,
    kDocumentPositionContainedBy = 0x10,
    kDocumentPositionImplementationSpecific = 0x20,
  };

  ~Node() override;

  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  virtual uint16_t GetNodeType() const = 0;
  virtual std::u16string NodeName() const = 0;
  bool IsConnected() const;
  Document* OwnerDocument() const;
  Node* ParentNode() const { return parent_.Get(); }
  Element* ParentElement() const;
  bool HasChildren() const { return first_child_; }
  Node* FirstChild() const { return first_child_.Get(); }
  Node* LastChild() const { return last_child_.Get(); }
  Node* PreviousSibling() const { return previous_sibling_.Get(); }
  Node* NextSibling() const { return next_sibling_.Get(); }
  virtual std::optional<std::u16string> NodeValue() const;
  virtual void SetNodeValue(const std::optional<std::u16string>& value,
                            bindings::ExceptionState& exception_state);
  virtual std::optional<std::u16string> TextContentForBinding() const;
  virtual void SetTextContentForBinding(
      const std::optional<std::u16string>& value,
      bindings::ExceptionState& exception_state);
  Node* GetRootNode(const bindings::GetRootNodeOptions& options);
  void Normalize();
  Node* CloneNode(bool deep, bindings::ExceptionState& exception_state);
  bool IsEqualNode(Node* other) const;
  bool IsSameNode(Node* other) const { return other == this; }
  uint16_t CompareDocumentPosition(Node* other) const;
  bool Contains(Node* other) const;
  std::optional<std::u16string> LookupPrefix(
      const std::optional<std::u16string>& namespace_uri) const;
  std::optional<std::u16string> LookupNamespaceURI(
      const std::optional<std::u16string>& prefix) const;
  bool IsDefaultNamespace(
      const std::optional<std::u16string>& namespace_uri) const;
  Node* InsertBefore(Node* node,
                     Node* child,
                     bindings::ExceptionState& exception_state);
  Node* AppendChild(Node* node, bindings::ExceptionState& exception_state);
  Node* ReplaceChild(Node* node,
                     Node* child,
                     bindings::ExceptionState& exception_state);
  Node* RemoveChild(Node* child, bindings::ExceptionState& exception_state);

  // The document's URL, as the base URL of a document without <base>.
  std::u16string BaseURI() const;
  // The same live list of the children on every read, as Blink keeps it.
  NodeList* ChildNodes();

  // The members of ParentNode, ChildNode and NonDocumentTypeChildNode.
  // The same live collection of the element children on every read.
  HTMLCollection* Children();
  Element* FirstElementChild() const;
  Element* LastElementChild() const;
  uint32_t ChildElementCount() const;
  Element* PreviousElementSibling() const;
  Element* NextElementSibling() const;
  void Prepend(const std::vector<bindings::NodeOrDOMString>& nodes,
               bindings::ExceptionState& exception_state);
  void Append(const std::vector<bindings::NodeOrDOMString>& nodes,
              bindings::ExceptionState& exception_state);
  void ReplaceChildren(const std::vector<bindings::NodeOrDOMString>& nodes,
                       bindings::ExceptionState& exception_state);
  void Before(const std::vector<bindings::NodeOrDOMString>& nodes,
              bindings::ExceptionState& exception_state);
  void After(const std::vector<bindings::NodeOrDOMString>& nodes,
             bindings::ExceptionState& exception_state);
  void ReplaceWith(const std::vector<bindings::NodeOrDOMString>& nodes,
                   bindings::ExceptionState& exception_state);
  void Remove(bindings::ExceptionState& exception_state);
  Element* QuerySelector(const std::u16string& selectors,
                         bindings::ExceptionState& exception_state);

  // What the DOM and the parser use.

  // The node document.
  Document& GetDocument() const { return *document_; }
  bool IsElementNode() const { return GetNodeType() == kElementNode; }
  bool IsTextNode() const { return GetNodeType() == kTextNode; }
  bool IsCommentNode() const { return GetNodeType() == kCommentNode; }
  bool IsDocumentNode() const { return GetNodeType() == kDocumentNode; }
  bool IsDocumentTypeNode() const { return GetNodeType() == kDocumentTypeNode; }
  bool IsDocumentFragmentNode() const {
    return GetNodeType() == kDocumentFragmentNode;
  }
  bool IsCharacterDataNode() const;
  // Whether it can have children: a document, a document fragment or an
  // element.
  bool IsContainerNode() const {
    return IsDocumentNode() || IsElementNode() || IsDocumentFragmentNode();
  }

  // The topmost ancestor, or this node.
  Node& TreeRoot() const;
  bool IsInclusiveAncestorOf(const Node& node) const;
  // The same, going from a template's contents to the template: the DOM
  // standard's host-including inclusive ancestor, which insertion checks.
  bool IsHostIncludingInclusiveAncestorOf(const Node& node) const;
  // The next node in tree order that is `stay_within` or inside it, or null.
  Node* NextInTree(const Node* stay_within) const;
  // The same, past this node's children.
  Node* NextInTreeSkippingChildren(const Node* stay_within) const;

  // The data of the Text nodes inside this one, in tree order: the DOM
  // standard's descendant text content.
  std::u16string DescendantTextContent() const;
  // The data of its Text children: the child text content.
  std::u16string ChildTextContent() const;

  // Appends `child`, which has no parent and may come from another document,
  // as the parser and the DOM's own algorithms do, without a page's checks.
  void ParserAppendChild(Node* child);
  // Inserts `child`, which has no parent, before `next`, a child of this node.
  void ParserInsertBefore(Node* child, Node& next);
  // Takes `child` out of this node's children.
  void ParserRemoveChild(Node& child);
  // Takes out every child.
  void RemoveChildren();

  // A copy of this node, in `document`, with its children if `deep`.
  Node* Clone(Document& document, bool deep) const;

  // EventTarget:
  Node* ToNode() override;

  // A click's activation behaviour, as Blink's Node splits the DOM
  // Standard's hooks (event_dispatcher.h): whether the node has any, its
  // legacy-pre-activation behaviour before the listeners run, which may hand
  // the rest a state, such as a checkbox's before it toggled, then what runs
  // before DOMActivate, which may mark the click as handled, and what runs
  // after it. A canceled click still runs the part before DOMActivate, which
  // reverts what legacy-pre-activation behaviour did.
  virtual bool HasActivationBehavior() const;
  virtual EventDispatchHandlingState* LegacyPreActivationBehavior(Event& event);
  virtual void RunActivationBehaviorBeforeDOMActivate(
      Event& event,
      EventDispatchHandlingState* state) {}
  virtual void RunActivationBehavior(Event& event,
                                     EventDispatchHandlingState* state) {}
  // Whether it is a form control that is disabled, which gets no simulated
  // click.
  virtual bool IsDisabledFormControl() const;
  // What runs once nothing canceled a trusted event or a click at this node
  // or below it, as Blink's Node::DefaultEventHandler(): a click at its
  // target fires DOMActivate there, and one that a listener cancels marks
  // the click as handled.
  virtual void DefaultEventHandler(Event& event);
  // Fires the click of the HTML Standard's click(), as Blink's
  // DispatchSimulatedClick() does for a script.
  void DispatchSimulatedClick();

  // The registrations of the MutationObservers that observe the node
  // (mutation_observer.h), null until the first.
  NodeMutationObserverData* MutationObserverData() const {
    return mutation_observer_data_.Get();
  }
  NodeMutationObserverData& EnsureMutationObserverData();

  // Gives this node and those inside it `document` as their node document,
  // and runs DidMoveToNewDocument() on each that moves, as Blink's
  // Document::AdoptIfNeeded() does.
  void SetTreeDocument(Document& document);

 protected:
  // `document` is the node document, null for a Document itself, which then
  // calls SetDocumentToSelf().
  explicit Node(Document* document);

  void SetDocumentToSelf(Document* document);

  // A copy of this node alone, in `document`.
  virtual Node* CloneWithoutChildren(Document& document) const = 0;
  // The DOM standard's cloning steps, which Clone() runs on this node for its
  // `copy` after copying the children, as Blink's
  // CloneNonAttributePropertiesFrom() runs on the copy.
  virtual void CloningSteps(Node& copy, bool deep) const {}
  // What a node does once it and its tree move to another document, from
  // `old_document`, as Blink's Node::DidMoveToNewDocument().
  virtual void DidMoveToNewDocument(Document& old_document) {}

  // EventTarget: through the tree.
  DispatchEventResult DispatchEventInternal(Event* event) override;

 private:
  // The DOM standard's "ensure pre-insertion validity" with Blink's checks
  // and messages, for `nodes` inserted before `next` or in place of
  // `old_child`.
  bool EnsurePreInsertionValidity(
      const NodeVector& nodes,
      const Node* next,
      const Node* old_child,
      bindings::ExceptionState& exception_state) const;
  // Whether a node of `type` may be a child of this one.
  bool ChildTypeAllowed(uint16_t type) const;
  // Inserts `nodes`, which are valid here, before `next`, or at the end. A
  // document fragment among them gives its children, which leave it, as the
  // DOM standard's insert does.
  void InsertNodes(const NodeVector& nodes, Node* next);
  // Blink's ConvertNodeUnionsIntoNodes(): nodes for the arguments of append()
  // and the like, with a new Text node for each string. With more than one,
  // each leaves its parent first.
  NodeVector ConvertNodeUnionsIntoNodes(
      const std::vector<bindings::NodeOrDOMString>& nodes,
      bindings::ExceptionState& exception_state);
  // Blink's ContainerNode::InsertBefore() for a vector of nodes.
  void InsertNodeVector(const NodeVector& nodes,
                        Node* next,
                        bindings::ExceptionState& exception_state);

  cppgc::Member<Node> parent_;
  cppgc::Member<Node> first_child_;
  cppgc::Member<Node> last_child_;
  cppgc::Member<Node> previous_sibling_;
  cppgc::Member<Node> next_sibling_;
  cppgc::Member<Document> document_;
  cppgc::Member<NodeList> child_nodes_;
  cppgc::Member<HTMLCollection> children_;
  cppgc::Member<NodeMutationObserverData> mutation_observer_data_;
};

}  // namespace mitto

#endif  // MITTO_DOM_NODE_H_
