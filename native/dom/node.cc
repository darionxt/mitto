#include "mitto/dom/node.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "base/check.h"
#include "base/check_op.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/interfaces_without_class.h"
#include "mitto/bindings/known_gap.h"
#include "mitto/dom/attr.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/child_list_mutation_scope.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/document_type.h"
#include "mitto/dom/element.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_dispatcher.h"
#include "mitto/dom/html_collection.h"
#include "mitto/dom/mutation_observer.h"
#include "mitto/dom/node_list.h"
#include "mitto/dom/objects_behind.h"
#include "mitto/dom/selector_query.h"
#include "mitto/dom/text.h"
#include "mitto/dom/window.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

namespace {

constexpr char kHierarchyRequestError[] = "HierarchyRequestError";
constexpr char kNotFoundError[] = "NotFoundError";
constexpr char kUnsupportedMethod[] =
    "This node type does not support this method.";

bool IsNodeInNodes(const Node* node,
                   const std::vector<bindings::NodeOrDOMString>& nodes) {
  for (const bindings::NodeOrDOMString& item : nodes) {
    if (const auto* other = std::get_if<Node*>(&item);
        other && *other == node) {
      return true;
    }
  }
  return false;
}

// Blink's CheckReferenceChildParent().
bool CheckReferenceChildParent(const Node& parent,
                               const Node* next,
                               const Node* old_child,
                               bindings::ExceptionState& exception_state) {
  if (next && next->ParentNode() != &parent) {
    exception_state.ThrowDOMException(
        kNotFoundError,
        "The node before which the new node is to be inserted is not a child "
        "of this node.");
    return false;
  }
  if (old_child && old_child->ParentNode() != &parent) {
    exception_state.ThrowDOMException(
        kNotFoundError, "The node to be replaced is not a child of this node.");
    return false;
  }
  return true;
}

// The nodes that inserting `nodes` inserts: a document fragment's children in
// its place.
NodeVector ChildrenOfFragments(const NodeVector& nodes) {
  NodeVector result;
  for (const cppgc::Persistent<Node>& node : nodes) {
    if (!node->IsDocumentFragmentNode()) {
      result.push_back(node);
      continue;
    }
    for (Node* child = node->FirstChild(); child;
         child = child->NextSibling()) {
      result.emplace_back(child);
    }
  }
  return result;
}

// Blink's Document::CanAcceptChild(): what a document can hold, one element
// and one doctype, the doctype first.
bool DocumentCanAcceptChildren(const Document& document,
                               const NodeVector& nodes,
                               const Node* next,
                               const Node* old_child,
                               bindings::ExceptionState& exception_state) {
  if (old_child && nodes.size() == 1 &&
      old_child->GetNodeType() == nodes[0]->GetNodeType()) {
    return true;
  }
  const NodeVector inserted = ChildrenOfFragments(nodes);
  int doctypes = 0;
  int elements = 0;
  bool doctype_after_reference = false;
  bool element_after_reference = false;
  bool saw_reference = false;
  for (Node* child = document.FirstChild(); child;
       child = child->NextSibling()) {
    if (child == old_child) {
      saw_reference = true;
      continue;
    }
    if (child == next) {
      saw_reference = true;
    }
    if (child->IsDocumentTypeNode()) {
      ++doctypes;
      doctype_after_reference = saw_reference;
    } else if (child->IsElementNode()) {
      ++elements;
      element_after_reference = saw_reference;
    }
  }
  for (const cppgc::Persistent<Node>& node : inserted) {
    switch (node->GetNodeType()) {
      case Node::kCommentNode:
      case Node::kProcessingInstructionNode:
        break;
      case Node::kDocumentTypeNode:
        ++doctypes;
        if (elements > 0 && !element_after_reference) {
          exception_state.ThrowDOMException(
              kHierarchyRequestError,
              "Can't insert a doctype after the root element.");
          return false;
        }
        break;
      case Node::kElementNode:
        ++elements;
        if (doctype_after_reference) {
          exception_state.ThrowDOMException(
              kHierarchyRequestError,
              "Can't insert an element before a doctype.");
          return false;
        }
        break;
      default:
        exception_state.ThrowDOMException(
            kHierarchyRequestError,
            base::StrCat({"Nodes of type '",
                          base::UTF16ToUTF8(node->NodeName()),
                          "' may not be inserted inside nodes of type "
                          "'#document'."}));
        return false;
    }
  }
  if (elements > 1 || doctypes > 1) {
    exception_state.ThrowDOMException(
        kHierarchyRequestError,
        base::StrCat({"Only one ", elements > 1 ? "element" : "doctype",
                      " on document allowed."}));
    return false;
  }
  return true;
}

bool AreEqualNodes(const Node& a, const Node& b) {
  if (a.GetNodeType() != b.GetNodeType()) {
    return false;
  }
  switch (a.GetNodeType()) {
    case Node::kDocumentTypeNode: {
      const auto& doctype_a = static_cast<const DocumentType&>(a);
      const auto& doctype_b = static_cast<const DocumentType&>(b);
      if (doctype_a.Name() != doctype_b.Name() ||
          doctype_a.PublicId() != doctype_b.PublicId() ||
          doctype_a.SystemId() != doctype_b.SystemId()) {
        return false;
      }
      break;
    }
    case Node::kElementNode: {
      const auto& element_a = static_cast<const Element&>(a);
      const auto& element_b = static_cast<const Element&>(b);
      if (element_a.NamespaceURI() != element_b.NamespaceURI() ||
          element_a.Prefix() != element_b.Prefix() ||
          element_a.LocalName() != element_b.LocalName() ||
          element_a.attributes().size() != element_b.attributes().size()) {
        return false;
      }
      for (const Element::Attribute& attribute : element_a.attributes()) {
        const std::u16string* other = element_b.FindAttribute(attribute.name);
        if (!other || *other != attribute.value) {
          return false;
        }
      }
      break;
    }
    case Node::kAttributeNode: {
      const auto& attr_a = static_cast<const Attr&>(a);
      const auto& attr_b = static_cast<const Attr&>(b);
      if (attr_a.NamespaceURI() != attr_b.NamespaceURI() ||
          attr_a.LocalName() != attr_b.LocalName() ||
          attr_a.Value() != attr_b.Value()) {
        return false;
      }
      break;
    }
    case Node::kTextNode:
    case Node::kCommentNode:
      if (static_cast<const CharacterData&>(a).Data() !=
          static_cast<const CharacterData&>(b).Data()) {
        return false;
      }
      break;
    default:
      break;
  }
  const Node* child_a = a.FirstChild();
  const Node* child_b = b.FirstChild();
  while (child_a && child_b) {
    if (!AreEqualNodes(*child_a, *child_b)) {
      return false;
    }
    child_a = child_a->NextSibling();
    child_b = child_b->NextSibling();
  }
  return !child_a && !child_b;
}

}  // namespace

Node::Node(Document* document) : document_(document) {}

Node::~Node() = default;

void Node::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(parent_);
  visitor->Trace(first_child_);
  visitor->Trace(last_child_);
  visitor->Trace(previous_sibling_);
  visitor->Trace(next_sibling_);
  visitor->Trace(document_);
  visitor->Trace(child_nodes_);
  visitor->Trace(children_);
  visitor->Trace(mutation_observer_data_);
}

NodeMutationObserverData& Node::EnsureMutationObserverData() {
  if (!mutation_observer_data_) {
    mutation_observer_data_ =
        bindings::MakeGarbageCollected<NodeMutationObserverData>();
  }
  return *mutation_observer_data_;
}

std::u16string Node::BaseURI() const {
  return base::UTF8ToUTF16(GetDocument().BaseUrl().spec());
}

NodeList* Node::ChildNodes() {
  if (!child_nodes_) {
    child_nodes_ = NodeList::CreateChildNodes(this);
  }
  return child_nodes_.Get();
}

HTMLCollection* Node::Children() {
  if (!children_) {
    children_ = HTMLCollection::Create(this, HTMLCollection::Type::kChildren);
  }
  return children_.Get();
}

void Node::SetDocumentToSelf(Document* document) {
  document_ = document;
}

bool Node::IsCharacterDataNode() const {
  const uint16_t type = GetNodeType();
  return type == kTextNode || type == kCommentNode ||
         type == kProcessingInstructionNode || type == kCdataSectionNode;
}

bool Node::IsConnected() const {
  return TreeRoot().IsDocumentNode();
}

Document* Node::OwnerDocument() const {
  return IsDocumentNode() ? nullptr : document_.Get();
}

Element* Node::ParentElement() const {
  return parent_ && parent_->IsElementNode()
             ? static_cast<Element*>(parent_.Get())
             : nullptr;
}

std::optional<std::u16string> Node::NodeValue() const {
  return std::nullopt;
}

void Node::SetNodeValue(const std::optional<std::u16string>& value,
                        bindings::ExceptionState& exception_state) {
  // The DOM standard: setting it does nothing for a node whose value is null.
}

std::optional<std::u16string> Node::TextContentForBinding() const {
  if (IsElementNode() || IsDocumentFragmentNode()) {
    return DescendantTextContent();
  }
  return NodeValue();
}

void Node::SetTextContentForBinding(const std::optional<std::u16string>& value,
                                    bindings::ExceptionState& exception_state) {
  if (!IsElementNode() && !IsDocumentFragmentNode()) {
    SetNodeValue(value, exception_state);
    return;
  }
  // The DOM standard's "string replace all", one record for observers.
  const ChildListMutationScope mutation(*this);
  RemoveChildren();
  if (value && !value->empty()) {
    ParserAppendChild(Text::Create(GetDocument(), *value));
  }
}

Node* Node::GetRootNode(const bindings::GetRootNodeOptions& options) {
  // Without shadow trees, the shadow-including root is the root.
  return &TreeRoot();
}

void Node::Normalize() {
  // Blink's Node::normalize(): merge each Text node with the Text nodes that
  // follow it, and drop the empty ones.
  Node* node = FirstChild();
  while (node) {
    if (!node->IsTextNode()) {
      node = node->NextInTree(this);
      continue;
    }
    auto* text = static_cast<Text*>(node);
    Node* next = text->NextSibling();
    while (next && next->IsTextNode()) {
      Node* const after = next->NextSibling();
      // Blink's Text::MergeNextSiblingNodesIfPossible() takes an empty one
      // out without changing this node's data, which an observer would hear
      // of.
      if (const std::u16string& data = static_cast<Text*>(next)->Data();
          !data.empty()) {
        text->AppendData(data);
      }
      next->ParentNode()->ParserRemoveChild(*next);
      next = after;
    }
    if (text->Data().empty()) {
      Node* const following = text->NextInTreeSkippingChildren(this);
      text->ParentNode()->ParserRemoveChild(*text);
      node = following;
      continue;
    }
    node = text->NextInTree(this);
  }
}

Node* Node::CloneNode(bool deep, bindings::ExceptionState& exception_state) {
  return Clone(GetDocument(), deep);
}

Node* Node::Clone(Document& document, bool deep) const {
  Node* const copy = CloneWithoutChildren(document);
  if (deep && IsContainerNode()) {
    // A document's copy is its children's node document.
    Document& children_document =
        copy->IsDocumentNode() ? static_cast<Document&>(*copy) : document;
    const cppgc::Persistent<Node> held(copy);
    for (Node* child = FirstChild(); child; child = child->NextSibling()) {
      copy->ParserAppendChild(child->Clone(children_document, true));
    }
  }
  CloningSteps(*copy, deep);
  return copy;
}

bool Node::IsEqualNode(Node* other) const {
  return other && AreEqualNodes(*this, *other);
}

uint16_t Node::CompareDocumentPosition(Node* other) const {
  if (other == this) {
    return 0;
  }
  // Blink's Node::compareDocumentPosition(): an attribute stands for its
  // element, right after it and before its children.
  const Attr* const attr1 = GetNodeType() == kAttributeNode
                                ? static_cast<const Attr*>(this)
                                : nullptr;
  const Attr* const attr2 = other->GetNodeType() == kAttributeNode
                                ? static_cast<const Attr*>(other)
                                : nullptr;
  const Node* const start1 = attr1 ? attr1->OwnerElement() : this;
  const Node* const start2 = attr2 ? attr2->OwnerElement() : other;
  // Blink orders nodes of different trees by their addresses, so that the
  // answer stays the same for the two.
  const uint16_t disconnected =
      kDocumentPositionDisconnected | kDocumentPositionImplementationSpecific |
      (this > other ? kDocumentPositionPreceding : kDocumentPositionFollowing);
  if (!start1 || !start2 || &start1->TreeRoot() != &start2->TreeRoot()) {
    return disconnected;
  }
  if (attr1 && attr2 && start1 == start2) {
    // Two attributes of one element, in the order of its attributes.
    for (const Element::Attribute& attribute :
         static_cast<const Element*>(start1)->attributes()) {
      if (attr1->GetQualifiedName() == attribute.name) {
        return kDocumentPositionImplementationSpecific |
               kDocumentPositionFollowing;
      }
      if (attr2->GetQualifiedName() == attribute.name) {
        return kDocumentPositionImplementationSpecific |
               kDocumentPositionPreceding;
      }
    }
    return disconnected;
  }
  if (!attr1 && !attr2) {
    if (IsInclusiveAncestorOf(*other)) {
      return kDocumentPositionContainedBy | kDocumentPositionFollowing;
    }
    if (other->IsInclusiveAncestorOf(*this)) {
      return kDocumentPositionContains | kDocumentPositionPreceding;
    }
  } else if (!attr2) {
    // `other` holds this attribute when it holds or is its element, and
    // follows it when it is inside the element.
    if (other->IsInclusiveAncestorOf(*start1)) {
      return kDocumentPositionContains | kDocumentPositionPreceding;
    }
    if (start1->IsInclusiveAncestorOf(*other)) {
      return kDocumentPositionFollowing;
    }
  } else if (!attr1) {
    if (IsInclusiveAncestorOf(*start2)) {
      return kDocumentPositionContainedBy | kDocumentPositionFollowing;
    }
    if (start2->IsInclusiveAncestorOf(*this)) {
      return kDocumentPositionPreceding;
    }
  } else {
    if (start1->IsInclusiveAncestorOf(*start2)) {
      return kDocumentPositionFollowing;
    }
    if (start2->IsInclusiveAncestorOf(*start1)) {
      return kDocumentPositionPreceding;
    }
  }
  for (const Node* node = start2->NextInTree(nullptr); node;
       node = node->NextInTree(nullptr)) {
    if (node == start1) {
      return kDocumentPositionPreceding;
    }
  }
  return kDocumentPositionFollowing;
}

bool Node::Contains(Node* other) const {
  return other && IsInclusiveAncestorOf(*other);
}

std::optional<std::u16string> Node::LookupPrefix(
    const std::optional<std::u16string>& namespace_uri) const {
  // Blink's Node::lookupPrefix().
  if (!namespace_uri || namespace_uri->empty()) {
    return std::nullopt;
  }
  const Element* context = nullptr;
  switch (GetNodeType()) {
    case kElementNode:
      context = static_cast<const Element*>(this);
      break;
    case kDocumentNode:
      context = static_cast<const Document*>(this)->DocumentElement();
      break;
    case kDocumentFragmentNode:
    case kDocumentTypeNode:
      break;
    case kAttributeNode:
      context = static_cast<const Attr*>(this)->OwnerElement();
      break;
    default:
      context = ParentElement();
      break;
  }
  return context ? context->LocateNamespacePrefix(*namespace_uri)
                 : std::nullopt;
}

std::optional<std::u16string> Node::LookupNamespaceURI(
    const std::optional<std::u16string>& prefix) const {
  // Blink's Node::lookupNamespaceURI(): an empty prefix is none.
  const std::optional<std::u16string> locate =
      prefix && prefix->empty() ? std::nullopt : prefix;
  const Element* context = nullptr;
  switch (GetNodeType()) {
    case kElementNode:
      context = static_cast<const Element*>(this);
      break;
    case kDocumentNode:
      context = static_cast<const Document*>(this)->DocumentElement();
      break;
    case kDocumentFragmentNode:
    case kDocumentTypeNode:
      break;
    case kAttributeNode:
      context = static_cast<const Attr*>(this)->OwnerElement();
      break;
    default:
      context = ParentElement();
      break;
  }
  return context ? context->LocateNamespace(locate) : std::nullopt;
}

bool Node::IsDefaultNamespace(
    const std::optional<std::u16string>& namespace_uri) const {
  const std::optional<std::u16string> expected =
      namespace_uri && namespace_uri->empty() ? std::nullopt : namespace_uri;
  return LookupNamespaceURI(std::nullopt) == expected;
}

bool Node::ChildTypeAllowed(uint16_t type) const {
  if (!IsElementNode() && !IsDocumentFragmentNode()) {
    return false;
  }
  return type == kElementNode || type == kTextNode || type == kCommentNode ||
         type == kProcessingInstructionNode || type == kCdataSectionNode;
}

bool Node::EnsurePreInsertionValidity(
    const NodeVector& nodes,
    const Node* next,
    const Node* old_child,
    bindings::ExceptionState& exception_state) const {
  if (IsDocumentNode()) {
    return CheckReferenceChildParent(*this, next, old_child, exception_state) &&
           DocumentCanAcceptChildren(static_cast<const Document&>(*this), nodes,
                                     next, old_child, exception_state);
  }
  for (const cppgc::Persistent<Node>& node : nodes) {
    if (node->IsContainerNode() &&
        node->IsHostIncludingInclusiveAncestorOf(*this)) {
      exception_state.ThrowDOMException(
          kHierarchyRequestError, "The new child element contains the parent.");
      return false;
    }
  }
  if (!CheckReferenceChildParent(*this, next, old_child, exception_state)) {
    return false;
  }
  for (const cppgc::Persistent<Node>& node : ChildrenOfFragments(nodes)) {
    if (!ChildTypeAllowed(node->GetNodeType())) {
      exception_state.ThrowDOMException(
          kHierarchyRequestError,
          base::StrCat({"Nodes of type '", base::UTF16ToUTF8(node->NodeName()),
                        "' may not be inserted inside nodes of type '",
                        base::UTF16ToUTF8(NodeName()), "'."}));
      return false;
    }
  }
  return true;
}

void Node::InsertNodes(const NodeVector& nodes, Node* next) {
  // Blink's ContainerNode::InsertBefore(): the nodes leave their parents
  // first, each with a record of its own, and a document fragment all its
  // children at once. Then InsertNodeVector(): each goes in, with a record of
  // all of them for observers, and is told so before the next, and the
  // children changed and post-connection steps follow once all are in.
  const NodeVector children = ChildrenOfFragments(nodes);
  for (const cppgc::Persistent<Node>& node : nodes) {
    if (node->IsDocumentFragmentNode()) {
      node->RemoveChildren();
    } else if (Node* const parent = node->ParentNode()) {
      parent->ParserRemoveChild(*node);
    }
  }
  ChildListMutationScope mutation(*this);
  NodeVector post_connection;
  for (const cppgc::Persistent<Node>& node : children) {
    LinkChild(*node, next);
    mutation.ChildAdded(*node);
    NotifyNodeInserted(*node, post_connection);
  }
  DidInsertNodes(children, post_connection, /*by_parser=*/false);
}

Node* Node::InsertBefore(Node* node,
                         Node* child,
                         bindings::ExceptionState& exception_state) {
  if (!IsContainerNode()) {
    exception_state.ThrowDOMException(kHierarchyRequestError,
                                      kUnsupportedMethod);
    return nullptr;
  }
  if (!child) {
    return AppendChild(node, exception_state);
  }
  const NodeVector nodes = {node};
  if (!EnsurePreInsertionValidity(nodes, child, nullptr, exception_state)) {
    return node;
  }
  Node* next = child;
  if (next == node) {
    next = node->NextSibling();
  }
  InsertNodes(nodes, next);
  return node;
}

Node* Node::AppendChild(Node* node, bindings::ExceptionState& exception_state) {
  if (!IsContainerNode()) {
    exception_state.ThrowDOMException(kHierarchyRequestError,
                                      kUnsupportedMethod);
    return nullptr;
  }
  const NodeVector nodes = {node};
  if (!EnsurePreInsertionValidity(nodes, nullptr, nullptr, exception_state)) {
    return node;
  }
  InsertNodes(nodes, nullptr);
  return node;
}

Node* Node::ReplaceChild(Node* node,
                         Node* child,
                         bindings::ExceptionState& exception_state) {
  if (!IsContainerNode()) {
    exception_state.ThrowDOMException(kHierarchyRequestError,
                                      kUnsupportedMethod);
    return nullptr;
  }
  const NodeVector nodes = {node};
  if (!EnsurePreInsertionValidity(nodes, nullptr, child, exception_state)) {
    return child;
  }
  Node* next = child->NextSibling();
  if (next == node) {
    next = node->NextSibling();
  }
  if (Node* const parent = node->ParentNode()) {
    parent->ParserRemoveChild(*node);
  }
  // The child that goes and the node that comes are one record.
  const ChildListMutationScope mutation(*this);
  if (child->ParentNode() == this) {
    ParserRemoveChild(*child);
  }
  InsertNodes(nodes, next);
  return child;
}

Node* Node::RemoveChild(Node* child,
                        bindings::ExceptionState& exception_state) {
  if (!IsContainerNode()) {
    exception_state.ThrowDOMException(kNotFoundError, kUnsupportedMethod);
    return nullptr;
  }
  if (child->ParentNode() != this) {
    exception_state.ThrowDOMException(
        kNotFoundError, "The node to be removed is not a child of this node.");
    return nullptr;
  }
  ParserRemoveChild(*child);
  return child;
}

Element* Node::FirstElementChild() const {
  for (Node* child = FirstChild(); child; child = child->NextSibling()) {
    if (child->IsElementNode()) {
      return static_cast<Element*>(child);
    }
  }
  return nullptr;
}

Element* Node::LastElementChild() const {
  for (Node* child = LastChild(); child; child = child->PreviousSibling()) {
    if (child->IsElementNode()) {
      return static_cast<Element*>(child);
    }
  }
  return nullptr;
}

uint32_t Node::ChildElementCount() const {
  uint32_t count = 0;
  for (Node* child = FirstChild(); child; child = child->NextSibling()) {
    if (child->IsElementNode()) {
      ++count;
    }
  }
  return count;
}

Element* Node::PreviousElementSibling() const {
  for (Node* sibling = PreviousSibling(); sibling;
       sibling = sibling->PreviousSibling()) {
    if (sibling->IsElementNode()) {
      return static_cast<Element*>(sibling);
    }
  }
  return nullptr;
}

Element* Node::NextElementSibling() const {
  for (Node* sibling = NextSibling(); sibling;
       sibling = sibling->NextSibling()) {
    if (sibling->IsElementNode()) {
      return static_cast<Element*>(sibling);
    }
  }
  return nullptr;
}

NodeVector Node::ConvertNodeUnionsIntoNodes(
    const std::vector<bindings::NodeOrDOMString>& nodes,
    bindings::ExceptionState& exception_state) {
  NodeVector result;
  for (const bindings::NodeOrDOMString& item : nodes) {
    if (Node* const* node = std::get_if<Node*>(&item)) {
      if ((*node)->IsDocumentFragmentNode()) {
        // A fragment gives its children, which leave it, as in Blink, with
        // one record for its observers.
        const cppgc::Persistent<Node> fragment(*node);
        const ChildListMutationScope mutation(*fragment);
        while (Node* const child = fragment->FirstChild()) {
          result.emplace_back(child);
          fragment->ParserRemoveChild(*child);
        }
        continue;
      }
      result.emplace_back(*node);
    } else {
      result.emplace_back(
          Text::Create(GetDocument(), std::get<std::u16string>(item)));
    }
  }
  if (result.size() > 1) {
    // Each leaves its parent once, and a node given twice keeps its last place,
    // as in Blink, which erases the first of a pair.
    std::vector<Node*> seen;
    std::vector<Node*> duplicates;
    for (const cppgc::Persistent<Node>& node : result) {
      if (std::find(seen.begin(), seen.end(), node.Get()) == seen.end()) {
        seen.push_back(node.Get());
        if (Node* const parent = node->ParentNode()) {
          parent->ParserRemoveChild(*node);
        }
      } else {
        duplicates.push_back(node.Get());
      }
    }
    for (Node* duplicate : duplicates) {
      const auto first =
          std::find_if(result.begin(), result.end(),
                       [duplicate](const cppgc::Persistent<Node>& node) {
                         return node.Get() == duplicate;
                       });
      CHECK(first != result.end());
      result.erase(first);
    }
  }
  return result;
}

void Node::InsertNodeVector(const NodeVector& nodes,
                            Node* next,
                            bindings::ExceptionState& exception_state) {
  if (!EnsurePreInsertionValidity(nodes, next, nullptr, exception_state)) {
    return;
  }
  if (nodes.size() == 1 && next == nodes[0].Get()) {
    next = next->NextSibling();
  }
  InsertNodes(nodes, next);
}

void Node::Prepend(const std::vector<bindings::NodeOrDOMString>& nodes,
                   bindings::ExceptionState& exception_state) {
  if (!IsContainerNode()) {
    exception_state.ThrowDOMException(kHierarchyRequestError,
                                      kUnsupportedMethod);
    return;
  }
  const NodeVector converted =
      ConvertNodeUnionsIntoNodes(nodes, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  InsertNodeVector(converted, FirstChild(), exception_state);
}

void Node::Append(const std::vector<bindings::NodeOrDOMString>& nodes,
                  bindings::ExceptionState& exception_state) {
  if (!IsContainerNode()) {
    exception_state.ThrowDOMException(kHierarchyRequestError,
                                      kUnsupportedMethod);
    return;
  }
  const NodeVector converted =
      ConvertNodeUnionsIntoNodes(nodes, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  InsertNodeVector(converted, nullptr, exception_state);
}

void Node::ReplaceChildren(const std::vector<bindings::NodeOrDOMString>& nodes,
                           bindings::ExceptionState& exception_state) {
  if (!IsContainerNode()) {
    exception_state.ThrowDOMException(kHierarchyRequestError,
                                      kUnsupportedMethod);
    return;
  }
  const NodeVector converted =
      ConvertNodeUnionsIntoNodes(nodes, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  if (!EnsurePreInsertionValidity(converted, nullptr, nullptr,
                                  exception_state)) {
    return;
  }
  const ChildListMutationScope mutation(*this);
  RemoveChildren();
  InsertNodes(converted, nullptr);
}

void Node::Before(const std::vector<bindings::NodeOrDOMString>& nodes,
                  bindings::ExceptionState& exception_state) {
  Node* const parent = ParentNode();
  if (!parent) {
    return;
  }
  Node* viable_previous = PreviousSibling();
  while (viable_previous && IsNodeInNodes(viable_previous, nodes)) {
    viable_previous = viable_previous->PreviousSibling();
  }
  const NodeVector converted =
      ConvertNodeUnionsIntoNodes(nodes, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  parent->InsertNodeVector(
      converted,
      viable_previous ? viable_previous->NextSibling() : parent->FirstChild(),
      exception_state);
}

void Node::After(const std::vector<bindings::NodeOrDOMString>& nodes,
                 bindings::ExceptionState& exception_state) {
  Node* const parent = ParentNode();
  if (!parent) {
    return;
  }
  Node* viable_next = NextSibling();
  while (viable_next && IsNodeInNodes(viable_next, nodes)) {
    viable_next = viable_next->NextSibling();
  }
  const NodeVector converted =
      ConvertNodeUnionsIntoNodes(nodes, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  parent->InsertNodeVector(converted, viable_next, exception_state);
}

void Node::ReplaceWith(const std::vector<bindings::NodeOrDOMString>& nodes,
                       bindings::ExceptionState& exception_state) {
  Node* const parent = ParentNode();
  if (!parent) {
    return;
  }
  Node* viable_next = NextSibling();
  while (viable_next && IsNodeInNodes(viable_next, nodes)) {
    viable_next = viable_next->NextSibling();
  }
  const NodeVector converted =
      ConvertNodeUnionsIntoNodes(nodes, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  if (parent != ParentNode()) {
    parent->InsertNodeVector(converted, viable_next, exception_state);
    return;
  }
  // Blink's ContainerNode::ReplaceChild() for a vector of nodes.
  if (!parent->EnsurePreInsertionValidity(converted, nullptr, this,
                                          exception_state)) {
    return;
  }
  Node* next = NextSibling();
  if (converted.size() == 1 && next == converted[0].Get()) {
    next = next->NextSibling();
  }
  if (converted.size() == 1) {
    if (Node* const old_parent = converted[0]->ParentNode()) {
      old_parent->ParserRemoveChild(*converted[0]);
    }
  }
  const cppgc::Persistent<Node> self(this);
  const ChildListMutationScope mutation(*parent);
  if (ParentNode() == parent) {
    parent->ParserRemoveChild(*this);
  }
  parent->InsertNodes(converted, next);
}

void Node::Remove(bindings::ExceptionState& exception_state) {
  if (Node* const parent = ParentNode()) {
    parent->RemoveChild(this, exception_state);
  }
}

Element* Node::QuerySelector(const std::u16string& selectors,
                             bindings::ExceptionState& exception_state) {
  const std::optional<SelectorQuery> query =
      SelectorQuery::Parse(selectors, exception_state);
  if (!query) {
    return nullptr;
  }
  return query->QueryFirst(*this);
}

Node& Node::TreeRoot() const {
  const Node* node = this;
  while (node->ParentNode()) {
    node = node->ParentNode();
  }
  return const_cast<Node&>(*node);
}

bool Node::IsHostIncludingInclusiveAncestorOf(const Node& node) const {
  for (const Node* ancestor = &node; ancestor;) {
    if (ancestor == this) {
      return true;
    }
    if (ancestor->ParentNode()) {
      ancestor = ancestor->ParentNode();
    } else if (ancestor->IsDocumentFragmentNode()) {
      ancestor = static_cast<const DocumentFragment*>(ancestor)->Host();
    } else {
      ancestor = nullptr;
    }
  }
  return false;
}

bool Node::IsInclusiveAncestorOf(const Node& node) const {
  for (const Node* ancestor = &node; ancestor;
       ancestor = ancestor->ParentNode()) {
    if (ancestor == this) {
      return true;
    }
  }
  return false;
}

Node* Node::NextInTree(const Node* stay_within) const {
  if (Node* const child = FirstChild()) {
    return child;
  }
  return NextInTreeSkippingChildren(stay_within);
}

Node* Node::NextInTreeSkippingChildren(const Node* stay_within) const {
  for (const Node* node = this; node; node = node->ParentNode()) {
    if (node == stay_within) {
      return nullptr;
    }
    if (Node* const sibling = node->NextSibling()) {
      return sibling;
    }
  }
  return nullptr;
}

std::u16string Node::DescendantTextContent() const {
  std::u16string text;
  for (const Node* node = FirstChild(); node; node = node->NextInTree(this)) {
    if (node->IsTextNode()) {
      text += static_cast<const Text*>(node)->Data();
    }
  }
  return text;
}

std::u16string Node::ChildTextContent() const {
  std::u16string text;
  for (const Node* child = FirstChild(); child; child = child->NextSibling()) {
    if (child->IsTextNode()) {
      text += static_cast<const Text*>(child)->Data();
    }
  }
  return text;
}

void Node::SetTreeDocument(Document& document) {
  if (document_ == &document) {
    return;
  }
  const cppgc::Persistent<Document> old_document(document_.Get());
  for (Node* node = this; node; node = node->NextInTree(this)) {
    node->document_ = &document;
  }
  for (Node* node = this; node; node = node->NextInTree(this)) {
    node->DidMoveToNewDocument(*old_document);
  }
}

Node::InsertionNotificationRequest Node::InsertedInto(Node& insertion_point) {
  return InsertionNotificationRequest::kInsertionDone;
}

void Node::LinkChild(Node& child, Node* next) {
  CHECK(!child.ParentNode());
  if (!IsDocumentNode()) {
    child.SetTreeDocument(GetDocument());
  } else {
    child.SetTreeDocument(static_cast<Document&>(*this));
  }
  child.parent_ = this;
  if (!next) {
    child.previous_sibling_ = last_child_;
    if (last_child_) {
      last_child_->next_sibling_ = &child;
    } else {
      first_child_ = &child;
    }
    last_child_ = &child;
    return;
  }
  CHECK_EQ(next->ParentNode(), this);
  child.next_sibling_ = next;
  child.previous_sibling_ = next->previous_sibling_;
  if (next->previous_sibling_) {
    next->previous_sibling_->next_sibling_ = &child;
  } else {
    first_child_ = &child;
  }
  next->previous_sibling_ = &child;
}

void Node::UnlinkChild(Node& child) {
  CHECK_EQ(child.ParentNode(), this);
  if (child.previous_sibling_) {
    child.previous_sibling_->next_sibling_ = child.next_sibling_;
  } else {
    first_child_ = child.next_sibling_;
  }
  if (child.next_sibling_) {
    child.next_sibling_->previous_sibling_ = child.previous_sibling_;
  } else {
    last_child_ = child.previous_sibling_;
  }
  child.parent_ = nullptr;
  child.previous_sibling_ = nullptr;
  child.next_sibling_ = nullptr;
}

void Node::NotifyNodeInserted(Node& child, NodeVector& post_connection) {
  const bool connected = IsConnected();
  for (Node* node = &child; node; node = node->NextInTree(&child)) {
    // As Blink, a leaf that goes into a tree that is not connected is not
    // told.
    if (!connected && !node->IsContainerNode()) {
      continue;
    }
    if (node->InsertedInto(*this) ==
        InsertionNotificationRequest::
            kInsertionShouldCallDidNotifySubtreeInsertions) {
      post_connection.emplace_back(node);
    }
  }
}

void Node::DidInsertNodes(const NodeVector& children,
                          const NodeVector& post_connection,
                          bool by_parser) {
  for (const cppgc::Persistent<Node>& child : children) {
    ChildrenChanged(child->IsElementNode()
                        ? ChildrenChangeType::kElementInserted
                        : ChildrenChangeType::kNonElementInserted,
                    *child, by_parser);
  }
  for (const cppgc::Persistent<Node>& node : post_connection) {
    if (node->IsConnected()) {
      node->DidNotifySubtreeInsertionsToDocument();
    }
  }
}

void Node::ParserAppendChild(Node* child) {
  LinkChild(*child, nullptr);
  ChildListMutationScope(*this).ChildAdded(*child);
  NodeVector post_connection;
  NotifyNodeInserted(*child, post_connection);
  DidInsertNodes({child}, post_connection, /*by_parser=*/true);
}

void Node::ParserInsertBefore(Node* child, Node& next) {
  LinkChild(*child, &next);
  ChildListMutationScope(*this).ChildAdded(*child);
  NodeVector post_connection;
  NotifyNodeInserted(*child, post_connection);
  DidInsertNodes({child}, post_connection, /*by_parser=*/true);
}

void Node::ParserRemoveChild(Node& child) {
  const cppgc::Persistent<Node> held(&child);
  // Blink's ContainerNode::WillRemoveChild(): a record for observers, and
  // the subtree observers of its ancestors follow it.
  ChildListMutationScope(*this).WillRemoveChild(child);
  NotifyMutationObserversNodeWillDetach(child);
  UnlinkChild(child);
  // Blink's ContainerNode::NotifyNodeRemoved(): the node and each node inside
  // it, but a leaf that leaves a tree that is not connected.
  const bool connected = IsConnected();
  for (Node* node = &child; node; node = node->NextInTree(&child)) {
    if (!connected && !node->IsContainerNode()) {
      continue;
    }
    node->RemovedFrom(*this);
  }
  ChildrenChanged(child.IsElementNode()
                      ? ChildrenChangeType::kElementRemoved
                      : ChildrenChangeType::kNonElementRemoved,
                  child, /*by_parser=*/false);
}

void Node::RemoveChildren() {
  // One record for all of them, as Blink's WillRemoveChildren() makes.
  const ChildListMutationScope mutation(*this);
  while (Node* const child = FirstChild()) {
    ParserRemoveChild(*child);
  }
}

Node* Node::ToNode() {
  return this;
}

bool Node::HasActivationBehavior() const {
  return false;
}

EventDispatchHandlingState* Node::LegacyPreActivationBehavior(Event& event) {
  return nullptr;
}

bool Node::IsDisabledFormControl() const {
  return false;
}

DispatchEventResult Node::DispatchEventInternal(Event* event) {
  return EventDispatcher::DispatchEvent(*this, *event);
}

void Node::DispatchSimulatedClick() {
  EventDispatcher::DispatchSimulatedClick(*this);
}

void Node::DefaultEventHandler(Event& event) {
  if (event.Target() != this || event.Type() != u"click") {
    return;
  }
  // Blink's DispatchDOMActivateEvent(): a UIEvent that bubbles as the click
  // does, so that it runs no activation behaviour of an ancestor where the
  // click would not, with the click's detail, which a simulated click leaves
  // at 0. Canceling it marks the click as handled, which runs no activation
  // behaviour.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  bindings::EventInit init;
  init.bubbles = event.Bubbles();
  init.cancelable = true;
  init.composed = event.Composed();
  const cppgc::Persistent<Event> activate(CreateEventOfInterface(
      bindings::V8UIEvent::kWrapperTypeInfo, u"DOMActivate", init));
  activate->SetUnderlyingEvent(&event);
  activate->SetTrusted(true);
  if (!IsConnected()) {
    activate->SetCopyEventPathFromUnderlyingEvent();
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> dictionary = v8::Object::New(isolate);
  Window* const window = GetDocument().DefaultView();
  const v8::Local<v8::Value> wrapper = activate->ToV8(isolate);
  if (wrapper->IsObject() &&
      dictionary
          ->Set(context, v8::String::NewFromUtf8Literal(isolate, "view"),
                window ? window->ToV8(isolate)
                       : v8::Null(isolate).As<v8::Value>())
          .IsJust() &&
      dictionary
          ->Set(context, v8::String::NewFromUtf8Literal(isolate, "detail"),
                bindings::EventOfInterfaceAttribute(isolate, event, "detail"))
          .IsJust()) {
    bindings::InitializeEventOfInterface(isolate, wrapper.As<v8::Object>(),
                                         bindings::V8UIEvent::kWrapperTypeInfo,
                                         dictionary);
  }
  // UIEvent has no class, so its isTrusted is a Known Gap's, which gives
  // what is stored for it.
  bindings::SetEventOfInterfaceAttribute(isolate, *activate, "isTrusted",
                                         v8::True(isolate));
  if (EventDispatcher::DispatchEvent(*this, *activate) !=
      DispatchEventResult::kNotCanceled) {
    event.SetDefaultHandled();
  }
}

}  // namespace mitto
