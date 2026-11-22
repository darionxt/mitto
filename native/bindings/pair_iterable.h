// The iterator of an interface with a pair iterable declaration whose class
// has its behaviour, such as Headers' and URLSearchParams': an object of the
// iterator's interface, which the bindings make, with an iteration source of
// the class behind it, which gives each pair as the class finds it then, as
// Blink's PairSyncIterationSource does. Each result has done before value, as
// Blink's ESCreateIterResultObject() makes it, where V8's own iterators, which
// a value iterable's members are, have value first.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/iterable.h
// Upstream: third_party/blink/renderer/bindings/core/v8/iterable.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/sync_iterator_base.h

#ifndef MITTO_BINDINGS_PAIR_ITERABLE_H_
#define MITTO_BINDINGS_PAIR_ITERABLE_H_

#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-maybe.h"

namespace v8 {
class Context;
class Function;
class Isolate;
class Object;
class Value;
}  // namespace v8

namespace mitto::bindings {

struct WrapperTypeInfo;

// Which of a pair an iterator gives, as Blink's SyncIteratorBase::Kind: the
// key for keys(), the value for values(), and both for entries() and
// @@iterator.
enum class PairIterationKind {
  kKey,
  kValue,
  kKeyValue,
};

// What gives the pairs of one iteration, which the class makes.
class PairIterationSource
    : public cppgc::GarbageCollected<PairIterationSource> {
 public:
  PairIterationSource(const PairIterationSource&) = delete;
  PairIterationSource& operator=(const PairIterationSource&) = delete;
  virtual ~PairIterationSource();

  // The iterator's next(): {done, value} of the next pair, as `kind` has it,
  // or of undefined once there is none.
  v8::Local<v8::Object> Next(v8::Isolate* isolate, PairIterationKind kind);
  // forEach(): calls `callback` with each value, its key and `object`, and
  // `this_arg` as this, until there is no pair left. Returns false if a call
  // threw, which leaves the exception for the caller.
  bool ForEach(v8::Isolate* isolate,
               v8::Local<v8::Function> callback,
               v8::Local<v8::Value> this_arg,
               v8::Local<v8::Value> object);

  virtual void Trace(cppgc::Visitor* visitor) const {}

 protected:
  PairIterationSource();

  // The next pair, as the page sees its key and value, or false if there is
  // none left.
  virtual bool FetchNextItem(v8::Isolate* isolate,
                             v8::Local<v8::Value>* key,
                             v8::Local<v8::Value>* value) = 0;
};

// A new object of the iterator's interface `iterator`, with `source` behind
// it, giving `kind`.
v8::MaybeLocal<v8::Value> NewPairIterator(v8::Local<v8::Context> context,
                                          const WrapperTypeInfo& iterator,
                                          PairIterationSource* source,
                                          PairIterationKind kind);

// The source behind `iterator`, and what it gives in `kind`, or null for an
// object that is no iterator of a class's pair iterable.
PairIterationSource* PairIterationSourceOf(v8::Isolate* isolate,
                                           v8::Local<v8::Value> iterator,
                                           PairIterationKind* kind);

}  // namespace mitto::bindings

#endif  // MITTO_BINDINGS_PAIR_ITERABLE_H_
