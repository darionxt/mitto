// The Streams Standard's queue-with-sizes operations, on a queue that a
// controller holds, as Blink's QueueWithSizes is.
//
// Upstream: third_party/blink/renderer/core/streams/queue_with_sizes.h
// Upstream: third_party/blink/renderer/core/streams/queue_with_sizes.cc

#ifndef MITTO_DOM_QUEUE_WITH_SIZES_H_
#define MITTO_DOM_QUEUE_WITH_SIZES_H_

#include "mitto/dom/heap_deque.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class QueueWithSizes final : public cppgc::GarbageCollected<QueueWithSizes> {
 public:
  QueueWithSizes();

  // DequeueValue().
  v8::Local<v8::Value> DequeueValue(v8::Isolate* isolate);
  // EnqueueValueWithSize(), for a size that the caller has made a number.
  void EnqueueValueWithSize(v8::Isolate* isolate,
                            v8::Local<v8::Value> value,
                            double size,
                            bindings::ExceptionState& exception_state);
  // PeekQueueValue().
  v8::Local<v8::Value> PeekQueueValue(v8::Isolate* isolate);
  double TotalSize() const { return queue_total_size_; }
  bool IsEmpty() const { return queue_.empty(); }
  // ResetQueue().
  void ResetQueue();

  void Trace(cppgc::Visitor* visitor) const;

 private:
  class ValueSizePair;

  HeapDeque<ValueSizePair> queue_;
  double queue_total_size_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_QUEUE_WITH_SIZES_H_
