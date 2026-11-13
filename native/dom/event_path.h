// The path of an event that a node dispatches: the node and its ancestors, and
// then the window of a document, as Blink's EventPath works it out once for
// each dispatch.
//
// Each node of the path belongs to a tree, a TreeScopeEventContext, which
// holds the event's target as the listeners of that tree see it, and the path
// that composedPath() gives them. Without shadow trees every node is in its
// document's tree, so a path has one, whose target is the node that
// dispatches. Shadow trees add a context for each shadow root on the way, with
// the target retargeted to its host: TreeScopeRootOf() and ParentTreeScopeOf()
// are where they come in, and closed trees filter ComposedPath().
//
// Upstream: third_party/blink/renderer/core/dom/events/event_path.h
// Upstream: third_party/blink/renderer/core/dom/events/event_path.cc
// Upstream: third_party/blink/renderer/core/dom/events/node_event_context.cc
// Upstream: third_party/blink/renderer/core/dom/events/tree_scope_event_context.cc
// Upstream: third_party/blink/renderer/core/dom/events/window_event_context.cc

#ifndef MITTO_DOM_EVENT_PATH_H_
#define MITTO_DOM_EVENT_PATH_H_

#include <stddef.h>

#include <vector>

#include "mitto/dom/heap_vector.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Event;
class EventPath;
class EventTarget;
class Node;
class Window;

// One tree of the path, and the event's target as its nodes see it.
class TreeScopeEventContext final
    : public cppgc::GarbageCollected<TreeScopeEventContext> {
 public:
  // `root` is the root of the tree: a document, or the root of a tree that is
  // not in one.
  explicit TreeScopeEventContext(Node& root);

  void Trace(cppgc::Visitor* visitor) const;

  Node& Root() const { return *root_; }
  EventTarget* Target() const { return target_.Get(); }
  void SetTarget(EventTarget& target) { target_ = &target; }

  // What composedPath() gives a listener of this tree: the path's nodes that
  // it may see, then the window, if the path has one.
  std::vector<EventTarget*> ComposedPath(const EventPath& path) const;

 private:
  cppgc::Member<Node> root_;
  cppgc::Member<EventTarget> target_;
};

// One node of the path: the current target of its listeners, and its tree.
class NodeEventContext final
    : public cppgc::GarbageCollected<NodeEventContext> {
 public:
  explicit NodeEventContext(Node& node);

  void Trace(cppgc::Visitor* visitor) const;

  Node& GetNode() const { return *node_; }
  TreeScopeEventContext& GetTreeScopeEventContext() const {
    return *tree_scope_event_context_;
  }
  void SetTreeScopeEventContext(TreeScopeEventContext& context) {
    tree_scope_event_context_ = &context;
  }
  // The event's target as this node's tree sees it.
  EventTarget* Target() const { return tree_scope_event_context_->Target(); }
  // Whether the listeners here see the node that dispatches, at the target
  // phase, rather than capturing or bubbling.
  bool CurrentTargetSameAsTarget() const;

  // Sets the event's target and current target for this node, and runs the
  // node's listeners, as Blink's NodeEventContext::HandleLocalEvents() does.
  void HandleLocalEvents(Event& event) const;

 private:
  cppgc::Member<Node> node_;
  cppgc::Member<TreeScopeEventContext> tree_scope_event_context_;
  const bool invocation_target_in_shadow_tree_;
};

// The window at the end of the path, for an event at a node of a document
// that has one, but for `load`, which Blink never takes to the window.
class WindowEventContext final
    : public cppgc::GarbageCollected<WindowEventContext> {
 public:
  WindowEventContext(Event& event, const NodeEventContext& top);

  void Trace(cppgc::Visitor* visitor) const;

  Window* GetWindow() const { return window_.Get(); }
  // The event's target as the window sees it, also after the dispatch.
  EventTarget* Target() const { return target_.Get(); }

  // Runs the window's listeners, and returns whether the path has a window.
  bool HandleLocalEvents(Event& event) const;

 private:
  cppgc::Member<Window> window_;
  cppgc::Member<EventTarget> target_;
};

class EventPath final : public cppgc::GarbageCollected<EventPath> {
 public:
  // The path of `event` at `node`.
  EventPath(Node& node, Event* event);

  void Trace(cppgc::Visitor* visitor) const;

  bool IsEmpty() const { return node_event_contexts_.empty(); }
  size_t size() const { return node_event_contexts_.size(); }
  const NodeEventContext& at(size_t index) const {
    return *node_event_contexts_[index];
  }
  const NodeEventContext& TopNodeEventContext() const { return at(size() - 1); }

  // Makes the window's context, once the dispatch starts, as Blink's does.
  void EnsureWindowEventContext();
  const WindowEventContext& GetWindowEventContext() const {
    return *window_event_context_;
  }
  bool HasWindowEventContext() const { return window_event_context_; }

  // The root of the tree that `node` is in, and of the tree that holds that
  // tree, or null: a document's tree holds no other. Shadow trees change
  // these.
  static Node& TreeScopeRootOf(const Node& node);
  static Node* ParentTreeScopeOf(const Node& root);

 private:
  // Blink's CalculatePath(): the node and each parent in turn.
  void CalculatePath();
  // Blink's CalculateAdjustedTargets(): each node's tree, and the target that
  // tree sees.
  void CalculateAdjustedTargets();
  TreeScopeEventContext* EnsureTreeScopeEventContext(Node* current_target,
                                                     Node* root);

  cppgc::Member<Node> node_;
  cppgc::Member<Event> event_;
  HeapVector<NodeEventContext> node_event_contexts_;
  HeapVector<TreeScopeEventContext> tree_scope_event_contexts_;
  cppgc::Member<WindowEventContext> window_event_context_;
};

}  // namespace mitto

#endif  // MITTO_DOM_EVENT_PATH_H_
