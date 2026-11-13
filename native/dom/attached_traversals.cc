#include "mitto/dom/attached_traversals.h"

#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/character_data.h"
#include "mitto/dom/node.h"
#include "mitto/dom/node_iterator.h"
#include "mitto/dom/range.h"
#include "mitto/dom/text.h"

namespace mitto {

struct AttachedTraversals::Entry final : public cppgc::GarbageCollected<Entry> {
  Entry(Range* range, NodeIterator* iterator, Entry* next)
      : range(range), iterator(iterator), next(next) {}

  void Trace(cppgc::Visitor* visitor) const {
    visitor->Trace(range);
    visitor->Trace(iterator);
    visitor->Trace(next);
  }

  // One of them.
  cppgc::WeakMember<Range> range;
  cppgc::WeakMember<NodeIterator> iterator;
  cppgc::Member<Entry> next;
};

AttachedTraversals::AttachedTraversals() = default;
AttachedTraversals::~AttachedTraversals() = default;

void AttachedTraversals::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(first_);
}

template <typename Visit>
void AttachedTraversals::ForEach(Visit visit) {
  cppgc::Member<Entry>* link = &first_;
  while (Entry* const entry = link->Get()) {
    if (!entry->range && !entry->iterator) {
      *link = entry->next;
      continue;
    }
    // On the stack, where the collector finds them, while the page's code
    // that a change may run cannot drop them.
    Range* const range = entry->range.Get();
    NodeIterator* const iterator = entry->iterator.Get();
    visit(range, iterator);
    link = &entry->next;
  }
}

void AttachedTraversals::AttachRange(Range* range) {
  first_ = bindings::MakeGarbageCollected<Entry>(range, nullptr, first_.Get());
}

void AttachedTraversals::DetachRange(Range* range) {
  cppgc::Member<Entry>* link = &first_;
  while (Entry* const entry = link->Get()) {
    if (entry->range == range) {
      *link = entry->next;
      return;
    }
    link = &entry->next;
  }
}

void AttachedTraversals::AttachNodeIterator(NodeIterator* iterator) {
  first_ =
      bindings::MakeGarbageCollected<Entry>(nullptr, iterator, first_.Get());
}

void AttachedTraversals::NodeWillBeRemoved(Node& node) {
  ForEach([&node](Range* range, NodeIterator* iterator) {
    if (range) {
      range->NodeWillBeRemoved(node);
    }
    if (iterator) {
      iterator->NodeWillBeRemoved(node);
    }
  });
}

void AttachedTraversals::DidInsertNode(Node& node) {
  ForEach([&node](Range* range, NodeIterator*) {
    if (range) {
      range->DidInsertNode(node);
    }
  });
}

void AttachedTraversals::DidReplaceText(CharacterData& node,
                                        uint32_t offset,
                                        uint32_t count,
                                        uint32_t added) {
  ForEach([&](Range* range, NodeIterator*) {
    if (range) {
      range->DidReplaceText(node, offset, count, added);
    }
  });
}

void AttachedTraversals::DidSplitText(Text& node,
                                      Text& new_node,
                                      uint32_t offset) {
  ForEach([&](Range* range, NodeIterator*) {
    if (range) {
      range->DidSplitText(node, new_node, offset);
    }
  });
}

void AttachedTraversals::DidMergeText(Text& node, Text& next, uint32_t offset) {
  ForEach([&](Range* range, NodeIterator*) {
    if (range) {
      range->DidMergeText(node, next, offset);
    }
  });
}

}  // namespace mitto
