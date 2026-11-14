#include "mitto/dom/navigation_transition.h"

#include <tuple>
#include <utility>

#include "mitto/bindings/v8_navigation_transition.h"
#include "mitto/dom/navigation_destination.h"
#include "mitto/dom/navigation_history_entry.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

v8::Local<v8::Promise::Resolver> NewResolver(v8::Isolate* isolate) {
  return v8::Promise::Resolver::New(isolate->GetCurrentContext())
      .ToLocalChecked();
}

}  // namespace

// static
NavigationTransition* NavigationTransition::Create(
    std::u16string navigation_type,
    NavigationHistoryEntry* from,
    NavigationDestination* to) {
  return bindings::MakeGarbageCollected<NavigationTransition>(
      std::move(navigation_type), from, to);
}

NavigationTransition::NavigationTransition(std::u16string navigation_type,
                                           NavigationHistoryEntry* from,
                                           NavigationDestination* to)
    : navigation_type_(std::move(navigation_type)), from_(from), to_(to) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Promise::Resolver> committed = NewResolver(isolate);
  const v8::Local<v8::Promise::Resolver> finished = NewResolver(isolate);
  finished->GetPromise()->MarkAsHandled();
  committed_.Reset(isolate, committed);
  finished_.Reset(isolate, finished);
}

NavigationTransition::~NavigationTransition() = default;

const bindings::WrapperTypeInfo* NavigationTransition::GetWrapperTypeInfo()
    const {
  return &bindings::V8NavigationTransition::kWrapperTypeInfo;
}

void NavigationTransition::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(from_);
  visitor->Trace(to_);
  visitor->Trace(committed_);
  visitor->Trace(finished_);
}

v8::Local<v8::Promise> NavigationTransition::Committed() const {
  return committed_.Get(v8::Isolate::GetCurrent())->GetPromise();
}

v8::Local<v8::Promise> NavigationTransition::Finished() const {
  return finished_.Get(v8::Isolate::GetCurrent())->GetPromise();
}

void NavigationTransition::ResolveCommitted() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::ignore = committed_.Get(isolate)->Resolve(isolate->GetCurrentContext(),
                                                 v8::Undefined(isolate));
}

void NavigationTransition::ResolveFinished() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::ignore = finished_.Get(isolate)->Resolve(isolate->GetCurrentContext(),
                                                v8::Undefined(isolate));
}

void NavigationTransition::RejectFinished(v8::Local<v8::Value> reason) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Promise::Resolver> committed = committed_.Get(isolate);
  if (committed->GetPromise()->State() == v8::Promise::kPending) {
    std::ignore = committed->Reject(context, reason);
  }
  std::ignore = finished_.Get(isolate)->Reject(context, reason);
}

}  // namespace mitto
