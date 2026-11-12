#include "mitto/bindings/known_gap.h"

#include <string>

#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/idl_member_installer.h"
#include "mitto/bindings/interface_table.h"
#include "mitto/bindings/per_isolate_data.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-isolate.h"

// Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/interface.py

namespace mitto::bindings {

namespace {

const MemberInfo& MemberOf(const v8::FunctionCallbackInfo<v8::Value>& info) {
  return *static_cast<const MemberInfo*>(
      info.Data().As<v8::External>()->Value(kMemberInfoTag));
}

bool IsOwnersObject(const v8::FunctionCallbackInfo<v8::Value>& info,
                    const MemberInfo& member) {
  v8::Isolate* const isolate = info.GetIsolate();
  return PerIsolateData::From(isolate)->HasInstance(isolate, *member.owner,
                                                    info.This());
}

// What an attribute's accessor does: with [LegacyLenientThis], nothing for a
// receiver of another kind, as bind_gen's make_check_receiver() has it.
void Accessor(const v8::FunctionCallbackInfo<v8::Value>& info) {
  const MemberInfo& member = MemberOf(info);
  if ((member.flags & kMemberLenientThis) && !IsOwnersObject(info, member)) {
    return;
  }
  ExceptionState exception_state(info.GetIsolate());
  exception_state.ThrowTypeError(ExceptionMessages::NotImplemented());
}

}  // namespace

void KnownGapAttributeGetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  Accessor(info);
}

void KnownGapAttributeSetter(const v8::FunctionCallbackInfo<v8::Value>& info) {
  Accessor(info);
}

void KnownGapOperation(const v8::FunctionCallbackInfo<v8::Value>& info) {
  ExceptionState exception_state(info.GetIsolate());
  exception_state.ThrowTypeError(ExceptionMessages::NotImplemented());
}

void KnownGapPromiseOperation(const v8::FunctionCallbackInfo<v8::Value>& info) {
  const MemberInfo& member = MemberOf(info);
  if (!(member.flags & kMemberStatic) && !IsOwnersObject(info, member)) {
    RejectWithTypeError(info, "Illegal invocation");
    return;
  }
  // The error goes into the promise, so V8 adds no context to it: the message
  // carries its own.
  RejectWithTypeError(info, ExceptionMessages::FailedToExecute(
                                member.name, member.owner->interface_name,
                                ExceptionMessages::NotImplemented()));
}

void KnownGapConstructor(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  if (!info.IsConstructCall()) {
    ThrowTypeError(isolate, ExceptionMessages::ConstructorCalledAsFunction());
    return;
  }
  ExceptionState exception_state(isolate);
  exception_state.ThrowTypeError(ExceptionMessages::NotImplemented());
}

void KnownGapFactoryFunction(const v8::FunctionCallbackInfo<v8::Value>& info) {
  KnownGapConstructor(info);
}

}  // namespace mitto::bindings
