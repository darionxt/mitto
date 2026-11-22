#include "mitto/bindings/pair_iterable.h"

#include "mitto/bindings/async_iterable.h"
#include "mitto/bindings/known_gap.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/wrapper_type_info.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace mitto::bindings {
namespace {

// The C++ side of an iterator: its source, and what it gives.
class PairIterator final : public v8::Object::Wrappable {
 public:
  PairIterator(PairIterationSource* source, PairIterationKind kind)
      : source_(source), kind_(kind) {}

  const char* GetHumanReadableName() const override { return "PairIterator"; }

  void Trace(cppgc::Visitor* visitor) const override {
    v8::Object::Wrappable::Trace(visitor);
    visitor->Trace(source_);
  }

  PairIterationSource* source() const { return source_.Get(); }
  PairIterationKind kind() const { return kind_; }

 private:
  const cppgc::Member<PairIterationSource> source_;
  const PairIterationKind kind_;
};

}  // namespace

PairIterationSource::PairIterationSource() = default;
PairIterationSource::~PairIterationSource() = default;

v8::Local<v8::Object> PairIterationSource::Next(v8::Isolate* isolate,
                                                PairIterationKind kind) {
  v8::Local<v8::Value> key;
  v8::Local<v8::Value> value;
  if (!FetchNextItem(isolate, &key, &value)) {
    return CreateIterResultObject(isolate, v8::Undefined(isolate), true);
  }
  switch (kind) {
    case PairIterationKind::kKey:
      return CreateIterResultObject(isolate, key, false);
    case PairIterationKind::kValue:
      return CreateIterResultObject(isolate, value, false);
    case PairIterationKind::kKeyValue:
      break;
  }
  v8::Local<v8::Value> pair[] = {key, value};
  return CreateIterResultObject(isolate, v8::Array::New(isolate, pair, 2),
                                false);
}

bool PairIterationSource::ForEach(v8::Isolate* isolate,
                                  v8::Local<v8::Function> callback,
                                  v8::Local<v8::Value> this_arg,
                                  v8::Local<v8::Value> object) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Value> key;
  v8::Local<v8::Value> value;
  while (FetchNextItem(isolate, &key, &value)) {
    v8::Local<v8::Value> arguments[] = {value, key, object};
    if (callback->Call(context, this_arg, 3, arguments).IsEmpty()) {
      return false;
    }
  }
  return true;
}

v8::MaybeLocal<v8::Value> NewPairIterator(v8::Local<v8::Context> context,
                                          const WrapperTypeInfo& iterator,
                                          PairIterationSource* source,
                                          PairIterationKind kind) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> object;
  if (!NewObjectOfInterface(context, iterator).ToLocal(&object)) {
    return {};
  }
  v8::Object::Wrap<kPairIteratorTag>(
      isolate, object.As<v8::Object>(),
      MakeGarbageCollected<PairIterator>(source, kind));
  return object;
}

PairIterationSource* PairIterationSourceOf(v8::Isolate* isolate,
                                           v8::Local<v8::Value> iterator,
                                           PairIterationKind* kind) {
  if (!iterator->IsObject()) {
    return nullptr;
  }
  PairIterator* const wrappable =
      v8::Object::Unwrap<kPairIteratorTag, PairIterator>(
          isolate, iterator.As<v8::Object>());
  if (!wrappable) {
    return nullptr;
  }
  *kind = wrappable->kind();
  return wrappable->source();
}

}  // namespace mitto::bindings
