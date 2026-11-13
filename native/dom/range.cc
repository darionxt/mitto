#include "mitto/dom/range.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_range.h"
#include "mitto/dom/attached_traversals.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/document.h"
#include "mitto/dom/document_fragment.h"
#include "mitto/dom/element.h"
#include "mitto/dom/html_element.h"
#include "mitto/dom/html_names.h"
#include "mitto/dom/html_tree_builder.h"
#include "mitto/dom/text.h"
#include "mitto/dom/window.h"

namespace mitto {

namespace {

constexpr char kHierarchyRequestError[] = "HierarchyRequestError";
constexpr char kIndexSizeError[] = "IndexSizeError";
constexpr char kInvalidNodeTypeError[] = "InvalidNodeTypeError";
constexpr char kWrongDocumentError[] = "WrongDocumentError";

uint32_t NodeIndex(const Node& node) {
  uint32_t index = 0;
  for (const Node* sibling = node.PreviousSibling(); sibling;
       sibling = sibling->PreviousSibling()) {
    ++index;
  }
  return index;
}

Node* ChildAt(const Node& parent, uint32_t index) {
  Node* child = parent.FirstChild();
  for (; child && index; --index) {
    child = child->NextSibling();
  }
  return child;
}

std::string NameOf(const Node& node) {
  return base::UTF16ToUTF8(node.NodeName());
}

// The DOM standard's position of a boundary point relative to another, as
// Blink's ComparePositionsInDomTree() works it out: -1 before, 0 equal, 1
// after, or `disconnected` for points in different trees.
int16_t ComparePoints(const Node& node_a,
                      uint32_t offset_a,
                      const Node& node_b,
                      uint32_t offset_b,
                      bool& disconnected) {
  disconnected = false;
  if (&node_a.TreeRoot() != &node_b.TreeRoot()) {
    disconnected = true;
    return 0;
  }
  if (&node_a == &node_b) {
    return offset_a == offset_b ? 0 : (offset_a < offset_b ? -1 : 1);
  }
  if (node_a.IsInclusiveAncestorOf(node_b)) {
    const Node* child = &node_b;
    while (child->ParentNode() != &node_a) {
      child = child->ParentNode();
    }
    return NodeIndex(*child) < offset_a ? 1 : -1;
  }
  if (node_b.IsInclusiveAncestorOf(node_a)) {
    const Node* child = &node_a;
    while (child->ParentNode() != &node_b) {
      child = child->ParentNode();
    }
    return NodeIndex(*child) < offset_b ? -1 : 1;
  }
  const uint16_t position = const_cast<Node&>(node_a).CompareDocumentPosition(
      const_cast<Node*>(&node_b));
  return (position & Node::kDocumentPositionFollowing) ? -1 : 1;
}

int16_t ComparePoints(const Node& node_a,
                      uint32_t offset_a,
                      const Node& node_b,
                      uint32_t offset_b,
                      bindings::ExceptionState& exception_state) {
  bool disconnected;
  const int16_t result =
      ComparePoints(node_a, offset_a, node_b, offset_b, disconnected);
  if (disconnected) {
    exception_state.ThrowDOMException(
        kWrongDocumentError, "The two ranges are in separate tree scopes.");
    return 0;
  }
  return result;
}

Node* HighestAncestorUnderCommonRoot(Node* node, Node* common_root) {
  if (node == common_root) {
    return nullptr;
  }
  while (node->ParentNode() != common_root) {
    node = node->ParentNode();
  }
  return node;
}

Node* ChildOfCommonRootBeforeOffset(Node* container,
                                    uint32_t offset,
                                    Node* common_root) {
  if (!common_root->IsInclusiveAncestorOf(*container)) {
    return nullptr;
  }
  if (container == common_root) {
    return ChildAt(*container, offset);
  }
  while (container->ParentNode() != common_root) {
    container = container->ParentNode();
  }
  return container;
}

void DeleteCharacterData(CharacterData* data,
                         uint32_t start_offset,
                         uint32_t end_offset,
                         bindings::ExceptionState& exception_state) {
  if (data->Length() - end_offset) {
    data->DeleteData(end_offset, data->Length() - end_offset, exception_state);
  }
  if (start_offset) {
    data->DeleteData(0, start_offset, exception_state);
  }
}

}  // namespace

// static
Range* Range::Create() {
  return Create(Window::Current().GetDocument());
}

// static
Range* Range::Create(Document& document) {
  return bindings::MakeGarbageCollected<Range>(document);
}

// static
bindings::ScriptWrappable* Range::CreateForKnownGap(
    const bindings::WrapperTypeInfo& info) {
  return Create();
}

Range::Range(Document& document)
    : NodeRange(&document, 0, &document, 0), owner_document_(&document) {
  document.Traversals().AttachRange(this);
}

Range::~Range() = default;

const bindings::WrapperTypeInfo* Range::GetWrapperTypeInfo() const {
  return &bindings::V8Range::kWrapperTypeInfo;
}

void Range::Trace(cppgc::Visitor* visitor) const {
  NodeRange::Trace(visitor);
  visitor->Trace(owner_document_);
}

void Range::SetDocument(Document& document) {
  owner_document_->Traversals().DetachRange(this);
  owner_document_ = &document;
  start_container_ = &document;
  start_offset_ = 0;
  end_container_ = &document;
  end_offset_ = 0;
  document.Traversals().AttachRange(this);
}

Node* Range::CommonAncestorContainer() const {
  for (Node* node = start_container_.Get(); node; node = node->ParentNode()) {
    if (node->IsInclusiveAncestorOf(*end_container_)) {
      return node;
    }
  }
  return nullptr;
}

void Range::CheckNodeWithOffset(
    Node* node,
    uint32_t offset,
    bindings::ExceptionState& exception_state) const {
  switch (node->GetNodeType()) {
    case Node::kDocumentTypeNode:
      exception_state.ThrowDOMException(
          kInvalidNodeTypeError, base::StrCat({"The node provided is of type '",
                                               NameOf(*node), "'."}));
      return;
    case Node::kCdataSectionNode:
    case Node::kCommentNode:
    case Node::kTextNode:
    case Node::kProcessingInstructionNode: {
      const uint32_t length = LengthOfContents(*node);
      if (offset > length) {
        exception_state.ThrowDOMException(
            kIndexSizeError,
            base::StrCat({"The offset ", base::NumberToString(offset),
                          " is larger than the node's length (",
                          base::NumberToString(length), ")."}));
      } else if (offset >
                 static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        exception_state.ThrowDOMException(
            kIndexSizeError,
            base::StrCat(
                {"The offset ", base::NumberToString(offset), " is invalid."}));
      }
      return;
    }
    default: {
      if (!offset) {
        return;
      }
      if (offset > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        exception_state.ThrowDOMException(
            kIndexSizeError,
            base::StrCat(
                {"The offset ", base::NumberToString(offset), " is invalid."}));
        return;
      }
      if (!ChildAt(*node, offset - 1)) {
        exception_state.ThrowDOMException(
            kIndexSizeError, base::StrCat({"There is no child at offset ",
                                           base::NumberToString(offset), "."}));
      }
      return;
    }
  }
}

void Range::CheckNodeBeforeOrAfter(
    Node* node,
    bindings::ExceptionState& exception_state) const {
  if (!node->ParentNode()) {
    exception_state.ThrowDOMException(kInvalidNodeTypeError,
                                      "the given Node has no parent.");
    return;
  }
  switch (node->GetNodeType()) {
    case Node::kAttributeNode:
    case Node::kDocumentFragmentNode:
    case Node::kDocumentNode:
      exception_state.ThrowDOMException(
          kInvalidNodeTypeError, base::StrCat({"The node provided is of type '",
                                               NameOf(*node), "'."}));
      return;
    default:
      break;
  }
  switch (node->TreeRoot().GetNodeType()) {
    case Node::kAttributeNode:
    case Node::kDocumentNode:
    case Node::kDocumentFragmentNode:
    case Node::kElementNode:
      return;
    default:
      exception_state.ThrowDOMException(
          kInvalidNodeTypeError, base::StrCat({"The node provided is of type '",
                                               NameOf(*node), "'."}));
      return;
  }
}

void Range::CollapseIfNeeded(bool moved_document, bool collapse_to_start) {
  bool disconnected;
  if (moved_document ||
      ComparePoints(*start_container_, start_offset_, *end_container_,
                    end_offset_, disconnected) > 0 ||
      disconnected) {
    Collapse(collapse_to_start);
  }
}

void Range::SetStart(Node* node,
                     uint32_t offset,
                     bindings::ExceptionState& exception_state) {
  bool moved_document = false;
  if (&node->GetDocument() != owner_document_) {
    SetDocument(node->GetDocument());
    moved_document = true;
  }
  CheckNodeWithOffset(node, offset, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  start_container_ = node;
  start_offset_ = offset;
  CollapseIfNeeded(moved_document, /*collapse_to_start=*/true);
}

void Range::SetEnd(Node* node,
                   uint32_t offset,
                   bindings::ExceptionState& exception_state) {
  bool moved_document = false;
  if (&node->GetDocument() != owner_document_) {
    SetDocument(node->GetDocument());
    moved_document = true;
  }
  CheckNodeWithOffset(node, offset, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  end_container_ = node;
  end_offset_ = offset;
  CollapseIfNeeded(moved_document, /*collapse_to_start=*/false);
}

void Range::SetStartBefore(Node* node,
                           bindings::ExceptionState& exception_state) {
  CheckNodeBeforeOrAfter(node, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  SetStart(node->ParentNode(), NodeIndex(*node), exception_state);
}

void Range::SetStartAfter(Node* node,
                          bindings::ExceptionState& exception_state) {
  CheckNodeBeforeOrAfter(node, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  SetStart(node->ParentNode(), NodeIndex(*node) + 1, exception_state);
}

void Range::SetEndBefore(Node* node,
                         bindings::ExceptionState& exception_state) {
  CheckNodeBeforeOrAfter(node, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  SetEnd(node->ParentNode(), NodeIndex(*node), exception_state);
}

void Range::SetEndAfter(Node* node, bindings::ExceptionState& exception_state) {
  CheckNodeBeforeOrAfter(node, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  SetEnd(node->ParentNode(), NodeIndex(*node) + 1, exception_state);
}

void Range::Collapse(bool to_start) {
  if (to_start) {
    end_container_ = start_container_;
    end_offset_ = start_offset_;
  } else {
    start_container_ = end_container_;
    start_offset_ = end_offset_;
  }
}

void Range::SelectNode(Node* node, bindings::ExceptionState& exception_state) {
  Node* const parent = node->ParentNode();
  if (!parent) {
    exception_state.ThrowDOMException(kInvalidNodeTypeError,
                                      "the given Node has no parent.");
    return;
  }
  switch (node->GetNodeType()) {
    case Node::kAttributeNode:
    case Node::kDocumentFragmentNode:
    case Node::kDocumentNode:
      exception_state.ThrowDOMException(
          kInvalidNodeTypeError, base::StrCat({"The node provided is of type '",
                                               NameOf(*node), "'."}));
      return;
    default:
      break;
  }
  // Blink's RangeBoundaryFastPath, which is stable.
  if (&node->GetDocument() != owner_document_) {
    SetDocument(node->GetDocument());
  }
  const uint32_t index = NodeIndex(*node);
  start_container_ = parent;
  start_offset_ = index;
  end_container_ = parent;
  end_offset_ = index + 1;
}

void Range::SelectNodeContents(Node* node,
                               bindings::ExceptionState& exception_state) {
  for (const Node* ancestor = node; ancestor;
       ancestor = ancestor->ParentNode()) {
    if (ancestor->IsDocumentTypeNode()) {
      exception_state.ThrowDOMException(
          kInvalidNodeTypeError, base::StrCat({"The node provided is of type '",
                                               NameOf(*node), "'."}));
      return;
    }
  }
  if (owner_document_ != &node->GetDocument()) {
    SetDocument(node->GetDocument());
  }
  start_container_ = node;
  start_offset_ = 0;
  end_container_ = node;
  end_offset_ = LengthOfContents(*node);
}

bool Range::HasSameRoot(const Node& node) const {
  return &node.GetDocument() == owner_document_ &&
         &node.TreeRoot() == &start_container_->TreeRoot();
}

bool Range::IsPointInRange(Node* node,
                           uint32_t offset,
                           bindings::ExceptionState& exception_state) const {
  if (!HasSameRoot(*node)) {
    return false;
  }
  CheckNodeWithOffset(node, offset, exception_state);
  if (exception_state.HadException()) {
    return false;
  }
  return ComparePoints(*node, offset, *start_container_, start_offset_,
                       exception_state) >= 0 &&
         !exception_state.HadException() &&
         ComparePoints(*node, offset, *end_container_, end_offset_,
                       exception_state) <= 0 &&
         !exception_state.HadException();
}

int16_t Range::ComparePoint(Node* node,
                            uint32_t offset,
                            bindings::ExceptionState& exception_state) const {
  if (!HasSameRoot(*node)) {
    exception_state.ThrowDOMException(
        kWrongDocumentError,
        "The node provided and the Range are not in the same tree.");
    return 0;
  }
  CheckNodeWithOffset(node, offset, exception_state);
  if (exception_state.HadException()) {
    return 0;
  }
  if (ComparePoints(*node, offset, *start_container_, start_offset_,
                    exception_state) < 0) {
    return -1;
  }
  if (exception_state.HadException()) {
    return 0;
  }
  if (ComparePoints(*node, offset, *end_container_, end_offset_,
                    exception_state) > 0 &&
      !exception_state.HadException()) {
    return 1;
  }
  return 0;
}

int16_t Range::CompareBoundaryPoints(
    uint16_t how,
    Range* source,
    bindings::ExceptionState& exception_state) const {
  if (how > kEndToStart) {
    exception_state.ThrowDOMException(
        "NotSupportedError",
        "The comparison method provided must be one of 'START_TO_START', "
        "'START_TO_END', 'END_TO_END', or 'END_TO_START'.");
    return 0;
  }
  Node* const this_container = CommonAncestorContainer();
  Node* const source_container = source->CommonAncestorContainer();
  if (this_container && source_container &&
      (&this_container->GetDocument() != &source_container->GetDocument() ||
       &this_container->TreeRoot() != &source_container->TreeRoot())) {
    exception_state.ThrowDOMException(
        kWrongDocumentError,
        "The source range is in a different document than this range.");
    return 0;
  }
  switch (how) {
    case kStartToStart:
      return ComparePoints(*start_container_, start_offset_,
                           *source->start_container_, source->start_offset_,
                           exception_state);
    case kStartToEnd:
      return ComparePoints(*end_container_, end_offset_,
                           *source->start_container_, source->start_offset_,
                           exception_state);
    case kEndToEnd:
      return ComparePoints(*end_container_, end_offset_,
                           *source->end_container_, source->end_offset_,
                           exception_state);
    case kEndToStart:
      return ComparePoints(*start_container_, start_offset_,
                           *source->end_container_, source->end_offset_,
                           exception_state);
  }
  NOTREACHED();
}

bool Range::IntersectsNode(Node* node,
                           bindings::ExceptionState& exception_state) const {
  if (!HasSameRoot(*node)) {
    return false;
  }
  Node* const parent = node->ParentNode();
  if (!parent) {
    return true;
  }
  const uint32_t index = NodeIndex(*node);
  bool disconnected;
  return ComparePoints(*parent, index, *end_container_, end_offset_,
                       disconnected) < 0 &&
         ComparePoints(*parent, index + 1, *start_container_, start_offset_,
                       disconnected) > 0;
}

Node* Range::FirstNode() const {
  if (start_container_->IsCharacterDataNode()) {
    return start_container_.Get();
  }
  if (Node* const child = ChildAt(*start_container_, start_offset_)) {
    return child;
  }
  return start_container_->NextInTreeSkippingChildren(nullptr);
}

Node* Range::PastLastNode() const {
  if (end_container_->IsCharacterDataNode()) {
    return end_container_->NextInTreeSkippingChildren(nullptr);
  }
  if (Node* const child = ChildAt(*end_container_, end_offset_)) {
    return child;
  }
  return end_container_->NextInTreeSkippingChildren(nullptr);
}

std::u16string Range::ToString() const {
  std::u16string text;
  Node* const past_last = PastLastNode();
  for (Node* node = FirstNode(); node && node != past_last;
       node = node->NextInTree(nullptr)) {
    if (node->GetNodeType() != Node::kTextNode &&
        node->GetNodeType() != Node::kCdataSectionNode) {
      continue;
    }
    const std::u16string& data = static_cast<CharacterData*>(node)->Data();
    const uint32_t length = static_cast<uint32_t>(data.size());
    const uint32_t start =
        node == start_container_ ? std::min(start_offset_, length) : 0;
    const uint32_t end = node == end_container_
                             ? std::min(std::max(start, end_offset_), length)
                             : length;
    text.append(data, start, end - start);
  }
  return text;
}

void Range::CheckExtractPrecondition(
    bindings::ExceptionState& exception_state) {
  if (!CommonAncestorContainer()) {
    return;
  }
  Node* const past_last = PastLastNode();
  for (Node* node = FirstNode(); node && node != past_last;
       node = node->NextInTree(nullptr)) {
    if (node->IsDocumentTypeNode()) {
      exception_state.ThrowDOMException(kHierarchyRequestError,
                                        "The Range contains a doctype node.");
      return;
    }
  }
}

void Range::DeleteContents(bindings::ExceptionState& exception_state) {
  ProcessContents(Action::kDelete, exception_state);
}

DocumentFragment* Range::ExtractContents(
    bindings::ExceptionState& exception_state) {
  CheckExtractPrecondition(exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  return ProcessContents(Action::kExtract, exception_state);
}

DocumentFragment* Range::CloneContents(
    bindings::ExceptionState& exception_state) {
  return ProcessContents(Action::kClone, exception_state);
}

// Blink's ProcessContents(), which DOM standard's "extract", "clone the
// contents" and deleteContents() share.
DocumentFragment* Range::ProcessContents(
    Action action,
    bindings::ExceptionState& exception_state) {
  // On the stack while the page's code that a change may run cannot drop
  // them.
  DocumentFragment* fragment = nullptr;
  if (action == Action::kExtract || action == Action::kClone) {
    fragment = DocumentFragment::Create(*owner_document_);
  }
  if (Collapsed()) {
    return fragment;
  }
  Node* const common_root = CommonAncestorContainer();
  if (start_container_ == end_container_) {
    ProcessContentsBetweenOffsets(action, fragment, start_container_.Get(),
                                  start_offset_, end_offset_, exception_state);
    return fragment;
  }

  Node* const original_start_container = start_container_.Get();
  const uint32_t original_start_offset = start_offset_;
  Node* const original_end_container = end_container_.Get();
  const uint32_t original_end_offset = end_offset_;

  Node* const partial_start =
      HighestAncestorUnderCommonRoot(original_start_container, common_root);
  Node* const partial_end =
      HighestAncestorUnderCommonRoot(original_end_container, common_root);

  Node* left_contents = nullptr;
  if (original_start_container != common_root &&
      common_root->IsInclusiveAncestorOf(*original_start_container)) {
    left_contents = ProcessContentsBetweenOffsets(
        action, nullptr, original_start_container, original_start_offset,
        LengthOfContents(*original_start_container), exception_state);
    left_contents = ProcessAncestorsAndTheirSiblings(
        action, original_start_container, /*forward=*/true, left_contents,
        common_root, exception_state);
  }

  Node* right_contents = nullptr;
  if (end_container_ != common_root &&
      common_root->IsInclusiveAncestorOf(*original_end_container)) {
    right_contents =
        ProcessContentsBetweenOffsets(action, nullptr, original_end_container,
                                      0, original_end_offset, exception_state);
    right_contents = ProcessAncestorsAndTheirSiblings(
        action, original_end_container, /*forward=*/false, right_contents,
        common_root, exception_state);
  }

  if (exception_state.HadException()) {
    return nullptr;
  }

  Node* process_start = ChildOfCommonRootBeforeOffset(
      original_start_container, original_start_offset, common_root);
  if (process_start && original_start_container != common_root) {
    process_start = process_start->NextSibling();
  }
  Node* const process_end = ChildOfCommonRootBeforeOffset(
      original_end_container, original_end_offset, common_root);
  if (!process_end &&
      !common_root->IsInclusiveAncestorOf(*original_end_container)) {
    process_start = nullptr;
  }

  if (action == Action::kExtract || action == Action::kDelete) {
    if (partial_start && common_root->IsInclusiveAncestorOf(*partial_start)) {
      SetStart(partial_start->ParentNode(), NodeIndex(*partial_start) + 1,
               exception_state);
    } else if (partial_end &&
               common_root->IsInclusiveAncestorOf(*partial_end)) {
      SetStart(partial_end->ParentNode(), NodeIndex(*partial_end),
               exception_state);
    }
    if (exception_state.HadException()) {
      return nullptr;
    }
    end_container_ = start_container_;
    end_offset_ = start_offset_;
  }

  if ((action == Action::kExtract || action == Action::kClone) &&
      left_contents) {
    fragment->AppendChild(left_contents, exception_state);
  }
  if (process_start) {
    NodeVector nodes;
    for (Node* node = process_start; node && node != process_end;
         node = node->NextSibling()) {
      nodes.emplace_back(node);
    }
    ProcessNodes(action, nodes, common_root, fragment, exception_state);
  }
  if ((action == Action::kExtract || action == Action::kClone) &&
      right_contents) {
    fragment->AppendChild(right_contents, exception_state);
  }
  return fragment;
}

Node* Range::ProcessContentsBetweenOffsets(
    Action action,
    DocumentFragment* fragment,
    Node* container,
    uint32_t start_offset,
    uint32_t end_offset,
    bindings::ExceptionState& exception_state) {
  Node* result = nullptr;
  switch (container->GetNodeType()) {
    case Node::kTextNode:
    case Node::kCdataSectionNode:
    case Node::kCommentNode:
    case Node::kProcessingInstructionNode: {
      auto* const data = static_cast<CharacterData*>(container);
      end_offset = std::min(end_offset, data->Length());
      if (action == Action::kExtract || action == Action::kClone) {
        auto* const copy = static_cast<CharacterData*>(
            container->CloneNode(/*deep=*/true, exception_state));
        DeleteCharacterData(copy, start_offset, end_offset, exception_state);
        if (fragment) {
          result = fragment;
          result->AppendChild(copy, exception_state);
        } else {
          result = copy;
        }
      }
      if (action == Action::kExtract || action == Action::kDelete) {
        data->DeleteData(start_offset, end_offset - start_offset,
                         exception_state);
      }
      break;
    }
    default: {
      if (action == Action::kExtract || action == Action::kClone) {
        result = fragment
                     ? fragment
                     : container->CloneNode(/*deep=*/false, exception_state);
      }
      NodeVector nodes;
      Node* node = ChildAt(*container, start_offset);
      for (uint32_t i = start_offset; node && i < end_offset;
           ++i, node = node->NextSibling()) {
        nodes.emplace_back(node);
      }
      ProcessNodes(action, nodes, container, result, exception_state);
      break;
    }
  }
  return result;
}

void Range::ProcessNodes(Action action,
                         const NodeVector& nodes,
                         Node* old_container,
                         Node* new_container,
                         bindings::ExceptionState& exception_state) {
  for (const cppgc::Persistent<Node>& node : nodes) {
    switch (action) {
      case Action::kDelete:
        old_container->RemoveChild(node.Get(), exception_state);
        break;
      case Action::kExtract:
        new_container->AppendChild(node.Get(), exception_state);
        break;
      case Action::kClone:
        new_container->AppendChild(
            node->CloneNode(/*deep=*/true, exception_state), exception_state);
        break;
    }
  }
}

Node* Range::ProcessAncestorsAndTheirSiblings(
    Action action,
    Node* container,
    bool forward,
    Node* cloned_container,
    Node* common_root,
    bindings::ExceptionState& exception_state) {
  NodeVector ancestors;
  for (Node* runner = container->ParentNode(); runner && runner != common_root;
       runner = runner->ParentNode()) {
    ancestors.emplace_back(runner);
  }
  NodeVector cloned_ancestors(ancestors.size());
  for (size_t i = ancestors.size(); i-- > 0;) {
    cloned_ancestors[i] =
        ancestors[i]->CloneNode(/*deep=*/false, exception_state);
  }
  Node* first_child_to_process =
      forward ? container->NextSibling() : container->PreviousSibling();
  for (size_t i = 0; i < ancestors.size(); ++i) {
    Node* const ancestor = ancestors[i].Get();
    if (action == Action::kExtract || action == Action::kClone) {
      if (Node* const cloned_ancestor = cloned_ancestors[i].Get()) {
        cloned_ancestor->AppendChild(cloned_container, exception_state);
        cloned_container = cloned_ancestor;
      }
    }
    NodeVector nodes;
    for (Node* child = first_child_to_process; child;
         child = forward ? child->NextSibling() : child->PreviousSibling()) {
      nodes.emplace_back(child);
    }
    for (const cppgc::Persistent<Node>& node : nodes) {
      Node* const child = node.Get();
      switch (action) {
        case Action::kDelete:
          if (ancestor == child->ParentNode()) {
            ancestor->RemoveChild(child, exception_state);
          }
          break;
        case Action::kExtract:
          if (forward) {
            cloned_container->AppendChild(child, exception_state);
          } else {
            cloned_container->InsertBefore(
                child, cloned_container->FirstChild(), exception_state);
          }
          break;
        case Action::kClone:
          if (forward) {
            cloned_container->AppendChild(
                child->CloneNode(/*deep=*/true, exception_state),
                exception_state);
          } else {
            cloned_container->InsertBefore(
                child->CloneNode(/*deep=*/true, exception_state),
                cloned_container->FirstChild(), exception_state);
          }
          break;
      }
    }
    first_child_to_process =
        forward ? ancestor->NextSibling() : ancestor->PreviousSibling();
  }
  return cloned_container;
}

void Range::InsertNode(Node* node, bindings::ExceptionState& exception_state) {
  Node& start_node = *start_container_;
  if (start_node.GetNodeType() == Node::kProcessingInstructionNode ||
      start_node.IsCommentNode()) {
    exception_state.ThrowDOMException(
        kHierarchyRequestError,
        base::StrCat({"Nodes of type '", NameOf(*node),
                      "' may not be inserted inside nodes of type '",
                      NameOf(start_node), "'."}));
    return;
  }
  const bool start_is_text = start_node.IsTextNode();
  if (start_is_text && !start_node.ParentNode()) {
    exception_state.ThrowDOMException(
        kHierarchyRequestError,
        "This operation would split a text node, but there's no parent into "
        "which to insert.");
    return;
  }
  if (&start_node == node) {
    exception_state.ThrowDOMException(
        kHierarchyRequestError,
        "Unable to insert a node into a Range starting from the node itself.");
    return;
  }
  Node* reference =
      start_is_text ? &start_node : ChildAt(start_node, start_offset_);
  Node* const parent = reference ? reference->ParentNode() : &start_node;
  if (!parent->EnsurePreInsertionValidity(node, reference, exception_state)) {
    return;
  }
  if (start_is_text) {
    reference = static_cast<Text&>(start_node)
                    .SplitText(start_offset_, exception_state);
    if (exception_state.HadException()) {
      return;
    }
  }
  if (node == reference) {
    reference = reference->NextSibling();
  }
  if (node->ParentNode()) {
    node->Remove(exception_state);
    if (exception_state.HadException()) {
      return;
    }
  }
  uint32_t new_offset =
      reference ? NodeIndex(*reference) : LengthOfContents(*parent);
  new_offset += node->IsDocumentFragmentNode() ? LengthOfContents(*node) : 1;
  parent->InsertBefore(node, reference, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  if (Collapsed()) {
    SetEnd(parent, new_offset, exception_state);
  }
}

void Range::SurroundContents(Node* new_parent,
                             bindings::ExceptionState& exception_state) {
  Node* start_node = start_container_.Get();
  Node* end_node = end_container_.Get();
  if (start_node->IsTextNode()) {
    start_node = start_node->ParentNode();
  }
  if (end_node->IsTextNode()) {
    end_node = end_node->ParentNode();
  }
  if (start_node != end_node) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "The Range has partially selected a non-Text node.");
    return;
  }
  switch (new_parent->GetNodeType()) {
    case Node::kAttributeNode:
    case Node::kDocumentFragmentNode:
    case Node::kDocumentNode:
    case Node::kDocumentTypeNode:
      exception_state.ThrowDOMException(
          kInvalidNodeTypeError, base::StrCat({"The node provided is of type '",
                                               NameOf(*new_parent), "'."}));
      return;
    default:
      break;
  }
  DocumentFragment* const fragment = ExtractContents(exception_state);
  if (exception_state.HadException()) {
    return;
  }
  while (Node* const child = new_parent->FirstChild()) {
    new_parent->RemoveChild(child, exception_state);
    if (exception_state.HadException()) {
      return;
    }
  }
  InsertNode(new_parent, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  new_parent->AppendChild(fragment, exception_state);
  if (exception_state.HadException()) {
    return;
  }
  SelectNode(new_parent, exception_state);
}

Range* Range::CloneRange() const {
  Range* const range = Create(*owner_document_);
  range->start_container_ = start_container_;
  range->start_offset_ = start_offset_;
  range->end_container_ = end_container_;
  range->end_offset_ = end_offset_;
  return range;
}

// DOM Parsing's createContextualFragment(), as Blink's runs it: the markup
// parsed as innerHTML would parse it for the start's element, or the body
// for none or the html element, with html, head and body elements dropped and
// their children kept.
DocumentFragment* Range::CreateContextualFragment(
    const std::u16string& fragment_markup,
    bindings::ExceptionState& exception_state) {
  Node* const node = start_container_.Get();
  Element* element = nullptr;
  if (!start_offset_ &&
      (node->IsDocumentNode() || node->IsDocumentFragmentNode())) {
    element = nullptr;
  } else if (node->IsElementNode()) {
    element = static_cast<Element*>(node);
  } else {
    element = node->ParentElement();
  }
  Document& document = node->GetDocument();
  if (!element || element->HasHTMLTagName(u"html")) {
    element = document.Body();
    if (!element) {
      element = document.CreateElement(
          std::u16string(html_names::kXhtmlNamespace), std::nullopt, u"body");
    }
  }
  if (!element->GetDocument().IsHTMLDocument()) {
    // A document that is not HTML parses markup as XML, which the Runtime
    // does not have yet.
    exception_state.ThrowTypeError("Not implemented.");
    return nullptr;
  }
  DocumentFragment* const fragment =
      DocumentFragment::Create(element->GetDocument());
  for (const cppgc::Persistent<Node>& child :
       ParseHtmlFragment(*element, fragment_markup)) {
    fragment->ParserAppendChild(child.Get());
  }
  Node* next = nullptr;
  for (Node* child = fragment->FirstChild(); child; child = next) {
    next = child->NextSibling();
    if (!child->IsElementNode()) {
      continue;
    }
    auto* const child_element = static_cast<Element*>(child);
    if (!child_element->HasHTMLTagName(u"html") &&
        !child_element->HasHTMLTagName(u"head") &&
        !child_element->HasHTMLTagName(u"body")) {
      continue;
    }
    if (Node* const first = child_element->FirstChild()) {
      next = first;
    }
    // Blink's RemoveElementPreservingChildren().
    Node* const parent = child_element->ParentNode();
    while (Node* const grandchild = child_element->FirstChild()) {
      child_element->ParserRemoveChild(*grandchild);
      parent->ParserInsertBefore(grandchild, *child_element);
    }
    parent->ParserRemoveChild(*child_element);
  }
  return fragment;
}

void Range::NodeWillBeRemoved(Node& node) {
  Node* const parent = node.ParentNode();
  const uint32_t index = NodeIndex(node);
  if (node.IsInclusiveAncestorOf(*start_container_)) {
    start_container_ = parent;
    start_offset_ = index;
  } else if (start_container_ == parent && start_offset_ > index) {
    --start_offset_;
  }
  if (node.IsInclusiveAncestorOf(*end_container_)) {
    end_container_ = parent;
    end_offset_ = index;
  } else if (end_container_ == parent && end_offset_ > index) {
    --end_offset_;
  }
}

void Range::DidInsertNode(Node& node) {
  Node* const parent = node.ParentNode();
  const uint32_t index = NodeIndex(node);
  if (start_container_ == parent && start_offset_ > index) {
    ++start_offset_;
  }
  if (end_container_ == parent && end_offset_ > index) {
    ++end_offset_;
  }
}

void Range::DidReplaceText(CharacterData& node,
                           uint32_t offset,
                           uint32_t count,
                           uint32_t added) {
  const auto update = [&](const cppgc::Member<Node>& container,
                          uint32_t& boundary_offset) {
    if (container != &node || boundary_offset <= offset) {
      return;
    }
    if (boundary_offset <= offset + count) {
      boundary_offset = offset;
    } else {
      boundary_offset = boundary_offset - count + added;
    }
  };
  update(start_container_, start_offset_);
  update(end_container_, end_offset_);
}

void Range::DidSplitText(Text& node, Text& new_node, uint32_t offset) {
  Node* const parent = node.ParentNode();
  const uint32_t index_after = NodeIndex(node) + 1;
  const auto update = [&](cppgc::Member<Node>& container,
                          uint32_t& boundary_offset) {
    if (container == &node && boundary_offset > offset) {
      container = &new_node;
      boundary_offset -= offset;
    } else if (container == parent && boundary_offset == index_after) {
      ++boundary_offset;
    }
  };
  update(start_container_, start_offset_);
  update(end_container_, end_offset_);
}

void Range::DidMergeText(Text& node, Text& next, uint32_t offset) {
  Node* const parent = next.ParentNode();
  const uint32_t next_index = NodeIndex(next);
  const auto update = [&](cppgc::Member<Node>& container,
                          uint32_t& boundary_offset) {
    if (container == &next) {
      container = &node;
      boundary_offset += offset;
    } else if (container == parent && boundary_offset == next_index) {
      container = &node;
      boundary_offset = offset;
    }
  };
  update(start_container_, start_offset_);
  update(end_container_, end_offset_);
}

}  // namespace mitto
