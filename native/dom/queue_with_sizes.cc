#include "mitto/dom/queue_with_sizes.h"

#include <cmath>

#include "base/check.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace {

// IsFiniteNonNegativeNumber().
bool IsFiniteNonNegativeNumber(double v) {
  return std::isfinite(v) && v >= 0;
}

}  // namespace

class QueueWithSizes::ValueSizePair final
    : public cppgc::GarbageCollected<ValueSizePair> {
 public:
  ValueSizePair(v8::Local<v8::Value> value, double size, v8::Isolate* isolate)
      : value_(isolate, value), size_(size) {}

  v8::Local<v8::Value> Value(v8::Isolate* isolate) const {
    return value_.Get(isolate);
  }
  double Size() const { return size_; }

  void Trace(cppgc::Visitor* visitor) const { visitor->Trace(value_); }

 private:
  v8::TracedReference<v8::Value> value_;
  const double size_;
};

QueueWithSizes::QueueWithSizes() = default;

v8::Local<v8::Value> QueueWithSizes::DequeueValue(v8::Isolate* isolate) {
  CHECK(!queue_.empty());
  // Step 3. Let pair be the first element of container.[[queue]].
  ValueSizePair* const pair = queue_.front();
  // Step 5. Subtract pair.[[size]] from container.[[queueTotalSize]].
  queue_total_size_ -= pair->Size();
  const v8::Local<v8::Value> value = pair->Value(isolate);
  // Step 4. Remove pair from container.[[queue]].
  queue_.pop_front();
  // Step 6. If container.[[queueTotalSize]] < 0, set it to 0, which rounding
  // errors can make.
  if (queue_total_size_ < 0) {
    queue_total_size_ = 0;
  }
  // Step 7. Return pair.[[value]].
  return value;
}

void QueueWithSizes::EnqueueValueWithSize(
    v8::Isolate* isolate,
    v8::Local<v8::Value> value,
    double size,
    bindings::ExceptionState& exception_state) {
  // Step 3. If ! IsFiniteNonNegativeNumber(size) is false, throw a RangeError
  // exception.
  if (!IsFiniteNonNegativeNumber(size)) {
    exception_state.ThrowRangeError(
        "The return value of a queuing strategy's size function must be a "
        "finite, non-NaN, non-negative number");
    return;
  }
  // Step 4. Append Record {[[value]]: value, [[size]]: size} to
  // container.[[queue]].
  queue_.push_back(
      bindings::MakeGarbageCollected<ValueSizePair>(value, size, isolate));
  // Step 5. Add size to container.[[queueTotalSize]].
  queue_total_size_ += size;
}

v8::Local<v8::Value> QueueWithSizes::PeekQueueValue(v8::Isolate* isolate) {
  // Step 2. Assert: container.[[queue]] is not empty.
  CHECK(!queue_.empty());
  // Steps 3-4. Return the first pair's [[value]].
  return queue_.front()->Value(isolate);
}

void QueueWithSizes::ResetQueue() {
  // Steps 2-3. Empty container.[[queue]], and set
  // container.[[queueTotalSize]] to 0.
  queue_.clear();
  queue_total_size_ = 0;
}

void QueueWithSizes::Trace(cppgc::Visitor* visitor) const {
  queue_.Trace(visitor);
}

}  // namespace mitto
