#include "mitto/dom/module_script.h"

#include <array>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/module_record.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {
namespace {

v8::Local<v8::Value> SyntaxError(v8::Isolate* isolate,
                                 std::u16string_view text) {
  return v8::Exception::SyntaxError(bindings::ToV8String(isolate, text));
}

v8::Local<v8::Value> TypeError(v8::Isolate* isolate, std::u16string_view text) {
  return v8::Exception::TypeError(bindings::ToV8String(isolate, text));
}

// ModuleEvaluationRejectionCallback: reports why a module's evaluation
// rejected, as an error that nothing caught.
void ReportEvaluationRejection(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  bindings::ReportException(info.GetIsolate(), info[0]);
}

}  // namespace

ModuleScriptCreationParams::ModuleScriptCreationParams() = default;
ModuleScriptCreationParams::ModuleScriptCreationParams(
    const ModuleScriptCreationParams&) = default;
ModuleScriptCreationParams& ModuleScriptCreationParams::operator=(
    const ModuleScriptCreationParams&) = default;
ModuleScriptCreationParams::~ModuleScriptCreationParams() = default;

// static
const char* ModuleScriptCreationParams::ModuleTypeToString(ModuleType type) {
  switch (type) {
    case ModuleType::kJSON:
      return "JSON";
    case ModuleType::kCSS:
      return "CSS";
    case ModuleType::kJavaScriptOrWasm:
      return "JavaScript-or-Wasm";
    case ModuleType::kText:
      return "Text";
    case ModuleType::kInvalid:
      NOTREACHED();
  }
  NOTREACHED();
}

ModuleScript::ModuleScript(Modulator* settings_object,
                           v8::Local<v8::Module> record,
                           const GURL& source_url,
                           const GURL& base_url,
                           const ScriptFetchOptions& fetch_options)
    : settings_object_(settings_object),
      source_url_(source_url),
      base_url_(base_url),
      fetch_options_(fetch_options) {
  if (!record.IsEmpty()) {
    record_.Reset(v8::Isolate::GetCurrent(), record);
  }
}

ModuleScript::~ModuleScript() = default;

void ModuleScript::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(settings_object_);
  visitor->Trace(record_);
  visitor->Trace(parse_error_);
  visitor->Trace(error_to_rethrow_);
}

v8::Local<v8::Module> ModuleScript::V8Module() const {
  if (record_.IsEmpty()) {
    return v8::Local<v8::Module>();
  }
  return record_.Get(v8::Isolate::GetCurrent());
}

std::vector<ModuleRequest> ModuleScript::GetModuleRecordRequests() const {
  CHECK(!record_.IsEmpty());
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  return ModuleRecord::ModuleRequests(isolate, record_.Get(isolate));
}

v8::Local<v8::Value> ModuleScript::Instantiate() const {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  return ModuleRecord::Instantiate(isolate->GetCurrentContext(),
                                   record_.Get(isolate));
}

void ModuleScript::SetParseErrorAndClearRecord(v8::Local<v8::Value> error) {
  DCHECK(!error.IsEmpty());
  record_.Reset();
  parse_error_.Reset(v8::Isolate::GetCurrent(), error);
}

v8::Local<v8::Value> ModuleScript::CreateParseError() const {
  DCHECK(!parse_error_.IsEmpty());
  return parse_error_.Get(v8::Isolate::GetCurrent());
}

void ModuleScript::SetErrorToRethrow(v8::Local<v8::Value> error) {
  error_to_rethrow_.Reset(v8::Isolate::GetCurrent(), error);
}

v8::Local<v8::Value> ModuleScript::CreateErrorToRethrow() const {
  DCHECK(!error_to_rethrow_.IsEmpty());
  return error_to_rethrow_.Get(v8::Isolate::GetCurrent());
}

GURL ModuleScript::ResolveModuleSpecifier(std::u16string_view specifier,
                                          std::string* failure_reason) const {
  if (const auto found = specifier_to_url_cache_.find(specifier);
      found != specifier_to_url_cache_.end()) {
    return found->second;
  }
  GURL url = settings_object_->ResolveModuleSpecifier(specifier, base_url_,
                                                      failure_reason);
  // Only a success is kept, so that a failure says why each time.
  if (url.is_valid()) {
    specifier_to_url_cache_.emplace(std::u16string(specifier), url);
  }
  return url;
}

ModuleScript::EvaluationResult ModuleScript::RunScriptAndReturnValue(
    bool rethrow_errors) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  // Prepare to run script: the microtasks run once the outermost script
  // that the Runtime runs returns, which covers the report of an error below.
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  EvaluationResult result;
  if (HasErrorToRethrow()) {
    result.type = ResultType::kException;
    result.value = CreateErrorToRethrow();
  } else {
    const v8::Local<v8::Module> record = V8Module();
    CHECK(!record.IsEmpty());
    const v8::TryCatch try_catch(isolate);
    // No microtask checkpoint here: one comes only after the errors are
    // handled, so that it comes at the same time with top-level await and
    // without.
    v8::MaybeLocal<v8::Value> maybe_result = record->Evaluate(context);
    if (!try_catch.CanContinue()) {
      result.type = ResultType::kAborted;
      return result;
    }
    DCHECK(!try_catch.HasCaught());
    result.type = ResultType::kSuccess;
    result.value = maybe_result.ToLocalChecked();
  }
  if (!rethrow_errors) {
    // Report the exception once the evaluation's promise rejects, which for
    // an error to rethrow is a promise rejected with it, as Blink's
    // ScriptEvaluationResult::GetPromise() makes it.
    v8::Local<v8::Promise> promise;
    if (result.type == ResultType::kException) {
      v8::Local<v8::Promise::Resolver> resolver;
      if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
          resolver->Reject(context, result.value).IsNothing()) {
        return result;
      }
      promise = resolver->GetPromise();
    } else {
      promise = result.value.As<v8::Promise>();
    }
    v8::Local<v8::Function> report;
    if (v8::Function::New(context, &ReportEvaluationRejection)
            .ToLocal(&report)) {
      std::ignore = promise->Catch(context, report);
    }
  }
  return result;
}

void ModuleScript::RunScript() {
  const v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
  std::ignore = RunScriptAndReturnValue(/*rethrow_errors=*/false);
}

// static
JSModuleScript* JSModuleScript::Create(const ModuleScriptCreationParams& params,
                                       Modulator* modulator,
                                       const ScriptFetchOptions& options,
                                       int start_line,
                                       int start_column) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::TryCatch try_catch(isolate);
  v8::Local<v8::Module> result;
  std::ignore =
      ModuleRecord::Compile(isolate, params.source_text, params.source_url,
                            params.base_url, options, start_line, start_column)
          .ToLocal(&result);
  JSModuleScript* const script = bindings::MakeGarbageCollected<JSModuleScript>(
      modulator, result, params.source_url, params.base_url, options);
  // Step 7, a part of ParseModule(): the record's [[HostDefined]] is the
  // script.
  modulator->RegisterModuleScript(script);
  // Step 8: the parse error is the first error.
  if (try_catch.HasCaught()) {
    DCHECK(result.IsEmpty());
    script->SetParseErrorAndClearRecord(try_catch.Exception());
    return script;
  }
  // Step 9: each request must have no attribute but "type", resolve, and
  // name a module type that is allowed.
  for (const ModuleRequest& requested :
       ModuleRecord::ModuleRequests(isolate, result)) {
    v8::Local<v8::Value> error;
    std::u16string invalid_key;
    std::string failure_reason;
    if (requested.HasInvalidImportAttributeKey(&invalid_key)) {
      error = SyntaxError(isolate, base::StrCat({u"Invalid attribute key \"",
                                                 invalid_key, u"\"."}));
    } else if (!script
                    ->ResolveModuleSpecifier(requested.specifier,
                                             &failure_reason)
                    .is_valid()) {
      error = TypeError(isolate,
                        base::StrCat({u"Failed to resolve module specifier \"",
                                      requested.specifier, u"\". ",
                                      base::UTF8ToUTF16(failure_reason)}));
    } else if (modulator->ModuleTypeFromRequest(requested) ==
               ModuleType::kInvalid) {
      error = TypeError(
          isolate,
          base::StrCat({u"\"", requested.GetModuleTypeString().value_or(u""),
                        u"\" is not a valid module type."}));
    }
    if (!error.IsEmpty()) {
      script->SetParseErrorAndClearRecord(error);
      return script;
    }
  }
  return script;
}

JSModuleScript::JSModuleScript(Modulator* settings_object,
                               v8::Local<v8::Module> record,
                               const GURL& source_url,
                               const GURL& base_url,
                               const ScriptFetchOptions& fetch_options)
    : ModuleScript(settings_object,
                   record,
                   source_url,
                   base_url,
                   fetch_options) {}

// static
ValueWrapperSyntheticModuleScript*
ValueWrapperSyntheticModuleScript::CreateJSONWrapperSyntheticModuleScript(
    const ModuleScriptCreationParams& params,
    Modulator* settings_object) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::TryCatch try_catch(isolate);
  // Shared cross-origin, and not opaque, as CORS is enforced on modules.
  const v8::ScriptOrigin origin(
      bindings::ToV8StringFromUtf8(isolate,
                                   params.source_url.possibly_invalid_spec()),
      /*resource_line_offset=*/0, /*resource_column_offset=*/0,
      /*resource_is_shared_cross_origin=*/true, /*script_id=*/-1,
      /*source_map_url=*/v8::Local<v8::Value>(),
      /*resource_is_opaque=*/false, /*is_wasm=*/false, /*is_module=*/true);
  v8::Local<v8::Value> parsed_json;
  if (!v8::JSON::Parse(
           context, bindings::ToV8String(isolate, params.source_text), origin)
           .ToLocal(&parsed_json) ||
      try_catch.HasCaught()) {
    return CreateWithError(settings_object, params.source_url,
                           try_catch.Exception());
  }
  return CreateWithDefaultExport(parsed_json, settings_object,
                                 params.source_url);
}

// static
ValueWrapperSyntheticModuleScript*
ValueWrapperSyntheticModuleScript::CreateTextWrapperSyntheticModuleScript(
    const ModuleScriptCreationParams& params,
    Modulator* settings_object) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  return CreateWithDefaultExport(
      bindings::ToV8String(isolate, params.source_text), settings_object,
      params.source_url);
}

// static
ValueWrapperSyntheticModuleScript*
ValueWrapperSyntheticModuleScript::CreateWithDefaultExport(
    v8::Local<v8::Value> value,
    Modulator* settings_object,
    const GURL& source_url) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const auto export_names = std::to_array<v8::Local<v8::String>>(
      {bindings::ToV8StringFromUtf8(isolate, "default")});
  const v8::Local<v8::Module> module = v8::Module::CreateSyntheticModule(
      isolate,
      bindings::ToV8StringFromUtf8(isolate, source_url.possibly_invalid_spec()),
      export_names, &EvaluationSteps, value);
  // Its base URL and fetch options are null, which Blink's empty URL and
  // default options stand for.
  return bindings::MakeGarbageCollected<ValueWrapperSyntheticModuleScript>(
      settings_object, module, source_url, GURL(), ScriptFetchOptions());
}

// static
ValueWrapperSyntheticModuleScript*
ValueWrapperSyntheticModuleScript::CreateWithError(Modulator* settings_object,
                                                   const GURL& source_url,
                                                   v8::Local<v8::Value> error) {
  ValueWrapperSyntheticModuleScript* const script =
      bindings::MakeGarbageCollected<ValueWrapperSyntheticModuleScript>(
          settings_object, v8::Local<v8::Module>(), source_url, GURL(),
          ScriptFetchOptions());
  script->SetParseErrorAndClearRecord(error);
  return script;
}

ValueWrapperSyntheticModuleScript::ValueWrapperSyntheticModuleScript(
    Modulator* settings_object,
    v8::Local<v8::Module> record,
    const GURL& source_url,
    const GURL& base_url,
    const ScriptFetchOptions& fetch_options)
    : ModuleScript(settings_object,
                   record,
                   source_url,
                   base_url,
                   fetch_options) {}

// static
v8::MaybeLocal<v8::Promise> ValueWrapperSyntheticModuleScript::EvaluationSteps(
    v8::Local<v8::Context> context,
    v8::Local<v8::Module> module) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kDoNotRunMicrotasks);
  const v8::Local<v8::Value> export_value =
      module->GetSyntheticModuleHostDefinedOptions().As<v8::Value>();
  // Setting the default export never fails.
  CHECK(module
            ->SetSyntheticModuleExport(
                isolate, bindings::ToV8StringFromUtf8(isolate, "default"),
                export_value)
            .FromMaybe(false));
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::MaybeLocal<v8::Promise>();
  }
  CHECK(resolver->Resolve(context, v8::Undefined(isolate)).FromMaybe(false));
  return resolver->GetPromise();
}

}  // namespace mitto
