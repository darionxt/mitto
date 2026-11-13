#include "mitto/dom/node_iterator_base.h"

#include "base/auto_reset.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/node.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"

namespace mitto {

NodeIteratorBase::NodeIteratorBase(v8::Isolate* isolate,
                                   Node* root,
                                   uint32_t what_to_show,
                                   v8::Local<v8::Object> filter)
    : root_(root), what_to_show_(what_to_show) {
  if (!filter.IsEmpty()) {
    filter_.Reset(isolate, filter);
  }
}

NodeIteratorBase::~NodeIteratorBase() = default;

void NodeIteratorBase::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(root_);
  visitor->Trace(filter_);
}

v8::Local<v8::Object> NodeIteratorBase::Filter() const {
  return filter_.Get(v8::Isolate::GetCurrent());
}

std::optional<uint16_t> NodeIteratorBase::AcceptNode(
    Node* node,
    bindings::ExceptionState& exception_state) {
  if (active_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "Filter function can't be recursive");
    return std::nullopt;
  }
  // The node types count from 1, and whatToShow's bits from 0.
  if (!((1u << (node->GetNodeType() - 1)) & what_to_show_)) {
    return kFilterSkip;
  }
  if (filter_.IsEmpty()) {
    return kFilterAccept;
  }
  const base::AutoReset<bool> active(&active_, true);
  v8::Isolate* const isolate = exception_state.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Object> filter = filter_.Get(isolate);
  // Web IDL's "call a user object's operation": the filter itself if it can
  // be called, with no `this`, and otherwise its acceptNode, with the filter
  // for `this`.
  v8::Local<v8::Function> function;
  v8::Local<v8::Value> receiver = v8::Undefined(isolate);
  if (filter->IsFunction()) {
    function = filter.As<v8::Function>();
  } else {
    v8::Local<v8::Value> accept_node;
    if (!filter
             ->Get(context,
                   v8::String::NewFromUtf8Literal(isolate, "acceptNode"))
             .ToLocal(&accept_node)) {
      return std::nullopt;
    }
    if (!accept_node->IsFunction()) {
      bindings::ThrowTypeError(isolate,
                               "Failed to execute 'acceptNode' on "
                               "'NodeFilter': The provided callback is not "
                               "callable.");
      return std::nullopt;
    }
    function = accept_node.As<v8::Function>();
    receiver = filter;
  }
  v8::Local<v8::Value> arguments[] = {bindings::ToV8(isolate, node)};
  v8::Local<v8::Value> result;
  if (!function->Call(context, receiver, 1, arguments).ToLocal(&result)) {
    return std::nullopt;
  }
  uint16_t value;
  if (!bindings::FromV8Integer(isolate, result, &value)) {
    return std::nullopt;
  }
  return value;
}

}  // namespace mitto
