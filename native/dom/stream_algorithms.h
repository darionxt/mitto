// The algorithms that the Streams Standard keeps in a stream's or a
// controller's internal slots, such as [[pullAlgorithm]], as Blink's classes.
// An algorithm may hold a page's functions, so it lives on V8's C++ heap,
// where V8's collector follows it.
//
// Upstream: third_party/blink/renderer/core/streams/stream_algorithms.h

#ifndef MITTO_DOM_STREAM_ALGORITHMS_H_
#define MITTO_DOM_STREAM_ALGORITHMS_H_

#include <optional>

#include "base/containers/span.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace v8 {
class Isolate;
class Value;
}  // namespace v8

namespace mitto {

// A [[strategySizeAlgorithm]]: the size of a chunk, or nullopt if it threw.
class StrategySizeAlgorithm
    : public cppgc::GarbageCollected<StrategySizeAlgorithm> {
 public:
  virtual ~StrategySizeAlgorithm() = default;
  virtual std::optional<double> Run(v8::Isolate* isolate,
                                    v8::Local<v8::Value> chunk) = 0;
  virtual void Trace(cppgc::Visitor* visitor) const {}
};

// A start algorithm, which unlike the others may throw: an empty promise means
// that it did.
class StreamStartAlgorithm
    : public cppgc::GarbageCollected<StreamStartAlgorithm> {
 public:
  virtual ~StreamStartAlgorithm() = default;
  virtual v8::Local<v8::Promise> Run(v8::Isolate* isolate) = 0;
  virtual void Trace(cppgc::Visitor* visitor) const {}
};

// Any other algorithm: it takes arguments and returns a promise.
class StreamAlgorithm : public cppgc::GarbageCollected<StreamAlgorithm> {
 public:
  virtual ~StreamAlgorithm() = default;
  virtual v8::Local<v8::Promise> Run(
      v8::Isolate* isolate,
      base::span<v8::Local<v8::Value>> arguments) = 0;
  virtual void Trace(cppgc::Visitor* visitor) const {}
};

}  // namespace mitto

#endif  // MITTO_DOM_STREAM_ALGORITHMS_H_
