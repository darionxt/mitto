#include "mitto/dom/event_path.h"

#include "base/check.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_target.h"
#include "mitto/dom/node.h"
#include "mitto/dom/window.h"

namespace mitto {

TreeScopeEventContext::TreeScopeEventContext(Node& root) : root_(&root) {}

void TreeScopeEventContext::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(root_);
  visitor->Trace(target_);
}

std::vector<EventTarget*> TreeScopeEventContext::ComposedPath(
    const EventPath& path) const {
  // Blink's TreeScopeEventContext::EnsureEventPath(). Each node of the path is
  // in a tree that this one may see, while no tree is closed.
  std::vector<EventTarget*> targets;
  targets.reserve(path.size() + 1);
  for (size_t i = 0; i < path.size(); ++i) {
    targets.push_back(&path.at(i).GetNode());
  }
  if (path.HasWindowEventContext()) {
    if (Window* const window = path.GetWindowEventContext().GetWindow()) {
      targets.push_back(window);
    }
  }
  return targets;
}

NodeEventContext::NodeEventContext(Node& node)
    : node_(&node),
      // A node is in a shadow tree only once shadow trees come.
      invocation_target_in_shadow_tree_(false) {}

void NodeEventContext::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(node_);
  visitor->Trace(tree_scope_event_context_);
}

bool NodeEventContext::CurrentTargetSameAsTarget() const {
  return node_.Get() == Target();
}

void NodeEventContext::HandleLocalEvents(Event& event) const {
  event.SetTarget(Target());
  event.SetCurrentTarget(node_.Get());
  event.SetInvocationTargetInShadowTree(invocation_target_in_shadow_tree_);
  node_->FireEventListeners(&event);
}

WindowEventContext::WindowEventContext(Event& event,
                                       const NodeEventContext& top) {
  // Blink never takes load to the window, as Mozilla did not.
  if (event.Type() == u"load") {
    return;
  }
  // With ClearTargetOnlyIfInShadowTree, which is on for stable, Blink keeps
  // the target for a top node that is not a document too, which has no
  // window, but is not in a shadow tree. A Page has no shadow trees yet.
  Node& node = top.GetNode();
  if (node.IsDocumentNode()) {
    window_ = static_cast<Document&>(node).DefaultView();
  }
  target_ = top.Target();
}

void WindowEventContext::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(window_);
  visitor->Trace(target_);
}

bool WindowEventContext::HandleLocalEvents(Event& event) const {
  if (!window_) {
    return false;
  }
  event.SetTarget(target_.Get());
  event.SetCurrentTarget(window_.Get());
  window_->FireEventListeners(&event);
  return true;
}

EventPath::EventPath(Node& node, Event* event) : node_(&node), event_(event) {
  CalculatePath();
  CalculateAdjustedTargets();
}

void EventPath::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(node_);
  visitor->Trace(event_);
  node_event_contexts_.Trace(visitor);
  tree_scope_event_contexts_.Trace(visitor);
  visitor->Trace(window_event_context_);
}

// static
Node& EventPath::TreeScopeRootOf(const Node& node) {
  // Blink's Node::GetTreeScope(): a node in no shadow tree is in its
  // document's, also while it is not in the document.
  return node.GetDocument();
}

// static
Node* EventPath::ParentTreeScopeOf(const Node& root) {
  return nullptr;
}

void EventPath::CalculatePath() {
  // The DOM Standard's get the parent: a node's parent, and nothing past the
  // root here; the window comes with the window's context. A shadow root's
  // parent, its host for a composed event, comes with shadow trees.
  for (Node* current = node_.Get(); current; current = current->ParentNode()) {
    node_event_contexts_.push_back(
        bindings::MakeGarbageCollected<NodeEventContext>(*current));
  }
}

TreeScopeEventContext* EventPath::EnsureTreeScopeEventContext(
    Node* current_target,
    Node* root) {
  if (!root) {
    return nullptr;
  }
  for (size_t i = 0; i < tree_scope_event_contexts_.size(); ++i) {
    TreeScopeEventContext* const context = tree_scope_event_contexts_[i];
    if (&context->Root() == root) {
      if (!context->Target() && current_target) {
        context->SetTarget(*current_target);
      }
      return context;
    }
  }
  TreeScopeEventContext* const context =
      bindings::MakeGarbageCollected<TreeScopeEventContext>(*root);
  tree_scope_event_contexts_.push_back(context);
  // A tree inside another sees the target that the outer one sees, if the
  // path has been there; otherwise its first node of the path.
  TreeScopeEventContext* const parent =
      EnsureTreeScopeEventContext(nullptr, ParentTreeScopeOf(*root));
  if (parent && parent->Target()) {
    context->SetTarget(*parent->Target());
  } else if (current_target) {
    context->SetTarget(*current_target);
  }
  return context;
}

void EventPath::CalculateAdjustedTargets() {
  Node* last_root = nullptr;
  TreeScopeEventContext* last_context = nullptr;
  for (size_t i = 0; i < node_event_contexts_.size(); ++i) {
    NodeEventContext* const context = node_event_contexts_[i];
    Node& current = context->GetNode();
    Node& root = TreeScopeRootOf(current);
    if (&root != last_root) {
      last_context = EnsureTreeScopeEventContext(&current, &root);
    }
    CHECK(last_context);
    context->SetTreeScopeEventContext(*last_context);
    last_root = &root;
  }
}

void EventPath::EnsureWindowEventContext() {
  if (!window_event_context_) {
    window_event_context_ = bindings::MakeGarbageCollected<WindowEventContext>(
        *event_, TopNodeEventContext());
  }
}

}  // namespace mitto
