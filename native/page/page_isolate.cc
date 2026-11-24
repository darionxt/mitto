#include "mitto/page/page_isolate.h"

#include <stddef.h>
#include <stdint.h>

#include "base/no_destructor.h"
#include "base/system/sys_info.h"
#include "base/time/time.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/bindings/runtime_features.h"
#include "mitto/bindings/v8_dom_exception.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/time_clamper.h"
#include "mitto/page/wasm_response_extensions.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-wasm.h"

namespace mitto {
namespace {

// Blink's kWasmWireBytesLimit: the most that WebAssembly compiles at once on
// a main thread, as `new WebAssembly.Module()` and `new WebAssembly.Instance()`
// do.
constexpr size_t kWasmWireBytesLimit = 1 << 23;

constexpr uint64_t kBytesPerGib = uint64_t{1} << 30;

void ThrowRangeError(v8::Isolate* isolate, const char* message) {
  isolate->ThrowException(v8::Exception::RangeError(
      v8::String::NewFromUtf8(isolate, message).ToLocalChecked()));
}

// WasmModuleOverride(): returns true where it threw instead of V8.
bool WasmModuleOverride(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1) {
    return false;
  }
  const v8::Local<v8::Value> source = info[0];
  if ((source->IsArrayBuffer() &&
       source.As<v8::ArrayBuffer>()->ByteLength() > kWasmWireBytesLimit) ||
      (source->IsArrayBufferView() &&
       source.As<v8::ArrayBufferView>()->ByteLength() > kWasmWireBytesLimit)) {
    ThrowRangeError(
        info.GetIsolate(),
        "WebAssembly.Compile is disallowed on the main thread, if the buffer "
        "size is larger than 8MB. Use WebAssembly.compile, compile on a "
        "worker thread, or use the flag "
        "`--enable-features=WebAssemblyUnlimitedSyncCompilation`.");
    return true;
  }
  return false;
}

// WasmInstanceOverride().
bool WasmInstanceOverride(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1 || !info[0]->IsWasmModuleObject()) {
    return false;
  }
  v8::CompiledWasmModule compiled =
      info[0].As<v8::WasmModuleObject>()->GetCompiledModule();
  if (compiled.GetWireBytesRef().size() > kWasmWireBytesLimit) {
    ThrowRangeError(
        info.GetIsolate(),
        "WebAssembly.Instance is disallowed on the main thread, if the buffer "
        "size is larger than 8MB. Use WebAssembly.instantiate, or use the "
        "flag `--enable-features=WebAssemblyUnlimitedSyncCompilation`.");
    return true;
  }
  return false;
}

// SharedArrayBufferConstructorEnabledCallback(): a context has
// SharedArrayBuffer if its execution context allows its transfer, which a
// cross-origin isolated one does, as a Page has neither the enterprise policy
// nor the origin trial that also allow it.
bool SharedArrayBufferConstructorEnabled(v8::Local<v8::Context> context) {
  const ExecutionContext* const execution_context =
      ExecutionContext::From(v8::Isolate::GetCurrent(), context);
  return execution_context &&
         execution_context->SharedArrayBufferTransferAllowed();
}

// WasmCustomDescriptorsEnabledCallback(): WebAssembly's custom descriptors,
// whose V8 flag is off, are on for a context whose runtime-enabled feature is,
// which a Page's set decides, as it has no origin trials.
bool WasmCustomDescriptorsEnabled(v8::Local<v8::Context> context) {
  return bindings::PerIsolateData::From(v8::Isolate::GetCurrent())
      ->features()
      .IsEnabled(bindings::RuntimeFeature::kWebAssemblyCustomDescriptorsV2);
}

// IsDOMExceptionWrapper(): Error.isError() is true for a DOMException.
bool IsDOMExceptionWrapper(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  return bindings::PerIsolateData::From(isolate)->HasInstance(
      isolate, bindings::V8DOMException::kWrapperTypeInfo, object);
}

// FineTemporalHostSystemUTCEpochNanosecondsCallback(), for a cross-origin
// isolated context, with a clamper of its own.
int64_t FineTemporalNow(v8::Local<v8::Context> context) {
  static const base::NoDestructor<TimeClamper> clamper;
  const base::TimeDelta since_epoch =
      base::Time::Now() - base::Time::UnixEpoch();
  return clamper
      ->ClampTimeResolution(since_epoch,
                            /*cross_origin_isolated_capability=*/true)
      .InNanoseconds();
}

// CoarseTemporalHostSystemUTCEpochNanosecondsCallback().
int64_t CoarseTemporalNow(v8::Local<v8::Context> context) {
  static const base::NoDestructor<TimeClamper> clamper;
  const base::TimeDelta since_epoch =
      base::Time::Now() - base::Time::UnixEpoch();
  return clamper
      ->ClampTimeResolution(since_epoch,
                            /*cross_origin_isolated_capability=*/false)
      .InNanoseconds();
}

}  // namespace

void ConfigurePageIsolate(v8::Isolate::CreateParams& params, int memory_gib) {
  // gin's IsolateHolder reads the machine's memory and address space. The
  // memory is the Machine's, and the address space only sizes V8's region for
  // code, which no page sees.
  params.constraints.ConfigureDefaults(
      static_cast<uint64_t>(memory_gib) * kBytesPerGib,
      base::SysInfo::AmountOfVirtualMemory().InBytes());
  // V8PerIsolateData: a main thread's isolate refuses Atomics.wait().
  params.allow_atomics_wait = false;
}

void ConfigureWorkerIsolate(v8::Isolate::CreateParams& params, int memory_gib) {
  ConfigurePageIsolate(params, memory_gib);
  // V8PerIsolateData: a worker's isolate allows Atomics.wait().
  params.allow_atomics_wait = true;
}

void InitializeWorkerIsolate(v8::Isolate* isolate) {
  // V8Initializer::InitializeV8Common(): the limits of WasmModuleOverride()
  // and WasmInstanceOverride() hold on the main thread alone.
  isolate->SetWasmCustomDescriptorsEnabledCallback(
      &WasmCustomDescriptorsEnabled);
  isolate->SetSharedArrayBufferConstructorEnabledCallback(
      &SharedArrayBufferConstructorEnabled);
  isolate->SetIsJSApiWrapperNativeErrorCallback(&IsDOMExceptionWrapper);
  isolate->SetHostImportModuleDynamicallyCallback(&HostImportModuleDynamically);
  isolate->SetHostInitializeImportMetaObjectCallback(
      &HostInitializeImportMetaObject);
  InitializeWasmResponseExtensions(isolate);
  // InitializeWorker(): kWorkerMaxStackSize below the stack's current
  // position.
  constexpr uintptr_t kWorkerMaxStackSize = 500 * 1024;
  uintptr_t here = 0;
  const uintptr_t position = reinterpret_cast<uintptr_t>(&here);
  isolate->SetStackLimit(position - kWorkerMaxStackSize);
}

void InitializePageIsolate(v8::Isolate* isolate) {
  // V8Initializer::InitializeV8Common().
  isolate->SetWasmModuleCallback(&WasmModuleOverride);
  isolate->SetWasmInstanceCallback(&WasmInstanceOverride);
  isolate->SetWasmCustomDescriptorsEnabledCallback(
      &WasmCustomDescriptorsEnabled);
  isolate->SetSharedArrayBufferConstructorEnabledCallback(
      &SharedArrayBufferConstructorEnabled);
  isolate->SetIsJSApiWrapperNativeErrorCallback(&IsDOMExceptionWrapper);
  isolate->SetHostImportModuleDynamicallyCallback(&HostImportModuleDynamically);
  isolate->SetHostInitializeImportMetaObjectCallback(
      &HostInitializeImportMetaObject);
  InitializeWasmResponseExtensions(isolate);
}

void InitializePageContext(v8::Local<v8::Context> context) {
  // V8Initializer::InitializeContext(): Temporal.Now reads a clock of the
  // context's resolution.
  const ExecutionContext* const execution_context =
      ExecutionContext::From(v8::Isolate::GetCurrent(), context);
  context->SetTemporalHostSystemUTCEpochNanosecondsCallback(
      execution_context && execution_context->CrossOriginIsolatedCapability()
          ? &FineTemporalNow
          : &CoarseTemporalNow);
}

}  // namespace mitto
