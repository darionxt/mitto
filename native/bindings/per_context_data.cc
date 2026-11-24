#include "mitto/bindings/per_context_data.h"

#include "mitto/bindings/per_isolate_data.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto::bindings {

namespace {

// The context's embedder data that holds whether it is a secure context.
// Index 0 is Chrome's debugger's (v8-context.h), and 1 the objects that the
// context made for constructs (idl_member_installer.cc).
constexpr int kContextSecureIndex = 2;
// And the one that holds whether it is cross-origin isolated. 4 holds the
// execution context bound to it (execution_context.cc).
constexpr int kContextCrossOriginIsolatedIndex = 3;

}  // namespace

void SetSecureContext(v8::Local<v8::Context> context, bool secure) {
  context->SetEmbedderDataV2(
      kContextSecureIndex, v8::Boolean::New(v8::Isolate::GetCurrent(), secure));
}

bool IsSecureContext(v8::Local<v8::Context> context) {
  if (context.IsEmpty() || context->GetNumberOfEmbedderDataFields() <=
                               static_cast<uint32_t>(kContextSecureIndex)) {
    return true;
  }
  const v8::Local<v8::Data> data =
      context->GetEmbedderDataV2(kContextSecureIndex);
  if (!data->IsValue() || !data.As<v8::Value>()->IsBoolean()) {
    return true;
  }
  return data.As<v8::Boolean>()->Value();
}

void SetCrossOriginIsolated(v8::Local<v8::Context> context, bool isolated) {
  context->SetEmbedderDataV2(
      kContextCrossOriginIsolatedIndex,
      v8::Boolean::New(v8::Isolate::GetCurrent(), isolated));
}

bool IsCrossOriginIsolated(v8::Local<v8::Context> context) {
  if (context.IsEmpty() ||
      context->GetNumberOfEmbedderDataFields() <=
          static_cast<uint32_t>(kContextCrossOriginIsolatedIndex)) {
    return false;
  }
  const v8::Local<v8::Data> data =
      context->GetEmbedderDataV2(kContextCrossOriginIsolatedIndex);
  return data->IsValue() && data.As<v8::Value>()->IsBoolean() &&
         data.As<v8::Boolean>()->Value();
}

ReceiverRealmScope::ReceiverRealmScope(v8::Isolate* isolate,
                                       v8::Local<v8::Object> receiver)
    : isolate_(isolate),
      outer_(PerIsolateData::From(isolate)->receiver_realm_scope()),
      current_(isolate->GetCurrentContext()) {
  // A global proxy whose global was detached, as a frame's window that has
  // gone, has no creation context: its call makes wrappers in the current
  // one. Contexts share a security token where the one may reach the other.
  v8::Local<v8::Context> receiver_context;
  if (receiver->GetCreationContext(isolate).ToLocal(&receiver_context) &&
      (receiver_context == current_ ||
       receiver_context->GetSecurityToken()->StrictEquals(
           current_->GetSecurityToken()))) {
    receiver_ = receiver_context;
  }
  PerIsolateData::From(isolate)->set_receiver_realm_scope(this);
}

ReceiverRealmScope::ReceiverRealmScope(v8::Isolate* isolate)
    : isolate_(isolate),
      outer_(PerIsolateData::From(isolate)->receiver_realm_scope()),
      current_(isolate->GetCurrentContext()) {
  PerIsolateData::From(isolate)->set_receiver_realm_scope(this);
}

ReceiverRealmScope::~ReceiverRealmScope() {
  PerIsolateData::From(isolate_)->set_receiver_realm_scope(outer_);
}

// static
v8::Local<v8::Context> ReceiverRealmScope::CreationContext(
    v8::Isolate* isolate) {
  const v8::Local<v8::Context> current = isolate->GetCurrentContext();
  const ReceiverRealmScope* const scope =
      PerIsolateData::From(isolate)->receiver_realm_scope();
  if (!scope || scope->receiver_.IsEmpty() || scope->current_ != current) {
    return current;
  }
  return scope->receiver_;
}

}  // namespace mitto::bindings
