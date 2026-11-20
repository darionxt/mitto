#include "mitto/dom/module_record.h"

#include <utility>

#include "base/check.h"
#include "base/numerics/safe_conversions.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/referrer_script_info.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

// static
v8::MaybeLocal<v8::Module> ModuleRecord::Compile(
    v8::Isolate* isolate,
    std::u16string_view source,
    const GURL& source_url,
    const GURL& base_url,
    const ScriptFetchOptions& options,
    int start_line,
    int start_column) {
  // V8 holds no longer string, and throws as for any string it cannot make.
  if (source.size() > static_cast<size_t>(v8::String::kMaxLength)) {
    isolate->ThrowError("Invalid string length");
    return v8::MaybeLocal<v8::Module>();
  }
  const std::string& spec = source_url.possibly_invalid_spec();
  // CORS is enforced on module scripts, so their errors are never muted.
  v8::ScriptOrigin origin(
      bindings::ToV8StringFromUtf8(isolate, spec), start_line, start_column,
      /*resource_is_shared_cross_origin=*/true, /*script_id=*/-1,
      /*source_map_url=*/v8::Local<v8::Value>(), /*resource_is_opaque=*/false,
      /*is_wasm=*/false, /*is_module=*/true,
      ReferrerScriptInfo(base_url, options)
          .ToV8HostDefinedOptions(isolate, source_url));
  v8::ScriptCompiler::Source compile_source(
      bindings::ToV8String(isolate, source), origin);
  return v8::ScriptCompiler::CompileModule(isolate, &compile_source);
}

// static
v8::Local<v8::Value> ModuleRecord::Instantiate(v8::Local<v8::Context> context,
                                               v8::Local<v8::Module> record) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::TryCatch try_catch(isolate);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  DCHECK(!record.IsEmpty());
  bool success = false;
  if (!record->InstantiateModule(context, &ResolveModuleCallback)
           .To(&success) ||
      !success) {
    DCHECK(try_catch.HasCaught());
    return try_catch.Exception();
  }
  DCHECK(!try_catch.HasCaught());
  return v8::Local<v8::Value>();
}

// static
std::vector<ModuleRequest> ModuleRecord::ModuleRequests(
    v8::Isolate* isolate,
    v8::Local<v8::Module> record) {
  std::vector<ModuleRequest> requests;
  if (record.IsEmpty()) {
    return requests;
  }
  const v8::Local<v8::FixedArray> v8_module_requests =
      record->GetModuleRequests();
  const int length = v8_module_requests->Length();
  requests.reserve(base::checked_cast<size_t>(length));
  for (int i = 0; i < length; ++i) {
    const v8::Local<v8::ModuleRequest> v8_module_request =
        v8_module_requests->Get(i).As<v8::ModuleRequest>();
    std::u16string specifier;
    bindings::FromV8String(isolate, v8_module_request->GetSpecifier(),
                           &specifier);
    requests.emplace_back(
        std::move(specifier),
        ToImportAttributes(isolate, v8_module_request->GetImportAttributes(),
                           /*has_positions=*/true));
  }
  return requests;
}

// static
std::vector<ImportAttribute> ModuleRecord::ToImportAttributes(
    v8::Isolate* isolate,
    v8::Local<v8::FixedArray> v8_import_attributes,
    bool has_positions) {
  const int entry_size = has_positions ? 3 : 2;
  std::vector<ImportAttribute> import_attributes;
  const int count = v8_import_attributes->Length() / entry_size;
  import_attributes.reserve(base::checked_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    ImportAttribute attribute;
    bindings::FromV8String(
        isolate, v8_import_attributes->Get(i * entry_size).As<v8::Value>(),
        &attribute.key);
    bindings::FromV8String(
        isolate, v8_import_attributes->Get(i * entry_size + 1).As<v8::Value>(),
        &attribute.value);
    import_attributes.push_back(std::move(attribute));
  }
  return import_attributes;
}

// static
v8::MaybeLocal<v8::Module> ModuleRecord::ResolveModuleCallback(
    v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_attributes,
    v8::Local<v8::Module> referrer) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  Modulator* const modulator = Modulator::From(context);
  CHECK(modulator);
  std::u16string specifier_string;
  bindings::FromV8String(isolate, specifier, &specifier_string);
  const ModuleRequest module_request(
      std::move(specifier_string),
      ToImportAttributes(isolate, import_attributes, /*has_positions=*/true));
  return modulator->ResolveRecord(module_request, referrer);
}

}  // namespace mitto
