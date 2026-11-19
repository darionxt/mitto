#include "mitto/dom/dynamic_module_resolver.h"

#include <string>
#include <tuple>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/module_script.h"
#include "mitto/dom/referrer_script_info.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {
namespace {

v8::Local<v8::Value> TypeError(v8::Isolate* isolate, std::u16string_view text) {
  return v8::Exception::TypeError(bindings::ToV8String(isolate, text));
}

// ModuleResolutionSuccessCallback: the import()'s promise takes the module's
// namespace once the evaluation's promise fulfills. Its data is the resolver
// and the namespace.
void OnEvaluationFulfilled(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Array> data = info.Data().As<v8::Array>();
  v8::Local<v8::Value> resolver;
  v8::Local<v8::Value> module_namespace;
  if (!data->Get(context, 0).ToLocal(&resolver) ||
      !data->Get(context, 1).ToLocal(&module_namespace)) {
    return;
  }
  std::ignore =
      resolver.As<v8::Promise::Resolver>()->Resolve(context, module_namespace);
}

// ModuleResolutionFailureCallback: it takes the evaluation's error. Its data
// is the resolver.
void OnEvaluationRejected(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  std::ignore = info.Data().As<v8::Promise::Resolver>()->Reject(
      isolate->GetCurrentContext(), info[0]);
}

class DynamicImportTreeClient final : public ModuleTreeClient {
 public:
  DynamicImportTreeClient(const GURL& url,
                          Modulator& modulator,
                          v8::Local<v8::Promise::Resolver> resolver)
      : url_(url), modulator_(&modulator) {
    resolver_.Reset(v8::Isolate::GetCurrent(), resolver);
  }

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(modulator_);
    visitor->Trace(resolver_);
    ModuleTreeClient::Trace(visitor);
  }

  // Steps 2, 9 and 10 of HostLoadImportedModule.
  void NotifyModuleTreeLoadFinished(ModuleScript* module_script) override {
    // Nothing settles the promise of a context that has gone.
    if (!modulator_->HasValidContext()) {
      return;
    }
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handle_scope(isolate);
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Promise::Resolver> resolver = resolver_.Get(isolate);
    // Step 2: a graph that failed rejects with a TypeError.
    if (!module_script) {
      std::ignore = resolver->Reject(
          context,
          TypeError(
              isolate,
              base::StrCat({u"Failed to fetch dynamically imported module: ",
                            base::UTF8ToUTF16(url_.possibly_invalid_spec())})));
      return;
    }
    // Step 9: run the module script, rethrowing its errors.
    const ModuleScript::EvaluationResult result =
        module_script->RunScriptAndReturnValue(/*rethrow_errors=*/true);
    switch (result.type) {
      case ModuleScript::ResultType::kException:
        // A parse or instantiation error, which comes before any promise.
        std::ignore = resolver->Reject(context, result.value);
        break;
      case ModuleScript::ResultType::kNotRun:
      case ModuleScript::ResultType::kAborted:
        break;
      case ModuleScript::ResultType::kSuccess: {
        // Step 10: settle as the evaluation's promise does.
        const v8::Local<v8::Module> record = module_script->V8Module();
        const v8::Local<v8::Value> module_namespace =
            record->GetStatus() == v8::Module::kErrored
                ? v8::Undefined(isolate).As<v8::Value>()
                : record->GetModuleNamespace();
        v8::Local<v8::Value> fulfilled_data[] = {resolver, module_namespace};
        v8::Local<v8::Function> on_fulfilled;
        v8::Local<v8::Function> on_rejected;
        if (!v8::Function::New(context, &OnEvaluationFulfilled,
                               v8::Array::New(isolate, fulfilled_data,
                                              std::size(fulfilled_data)))
                 .ToLocal(&on_fulfilled) ||
            !v8::Function::New(context, &OnEvaluationRejected, resolver)
                 .ToLocal(&on_rejected)) {
          return;
        }
        std::ignore = result.value.As<v8::Promise>()->Then(
            context, on_fulfilled, on_rejected);
        break;
      }
    }
  }

 private:
  const GURL url_;
  cppgc::Member<Modulator> modulator_;
  v8::TracedReference<v8::Promise::Resolver> resolver_;
};

}  // namespace

DynamicModuleResolver::DynamicModuleResolver(Modulator& modulator)
    : modulator_(&modulator) {}

void DynamicModuleResolver::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(modulator_);
}

void DynamicModuleResolver::ResolveDynamically(
    const ModuleRequest& module_request,
    const ReferrerScriptInfo& referrer_info,
    v8::Local<v8::Promise::Resolver> resolver) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const ModuleType module_type =
      modulator_->ModuleTypeFromRequest(module_request);
  // Step 4.3: the base URL of the script that calls it, or, where there is
  // none, the window's API base URL.
  GURL base_url = referrer_info.BaseUrl();
  if (base_url.is_empty()) {
    base_url = modulator_->GetWindow()->GetDocument().BaseUrl();
  }
  // Wait for import maps: the document no longer acquires them.
  modulator_->SetAcquiringImportMapsState(
      Modulator::AcquiringImportMapsState::kAfterModuleScriptLoad);
  const GURL url = modulator_->ResolveModuleSpecifier(
      module_request.specifier, base_url, /*failure_reason=*/nullptr);
  // Step 6: a specifier that does not resolve, or a module type that is not
  // allowed, rejects with a TypeError.
  if (!url.is_valid() || module_type == ModuleType::kInvalid) {
    std::u16string message;
    if (!url.is_valid()) {
      message = base::StrCat({u"Failed to resolve module specifier '",
                              module_request.specifier, u"'"});
      if (referrer_info.BaseUrl().IsAboutBlank() && base_url.IsAboutBlank()) {
        message +=
            u". The base URL is about:blank because import() is called from "
            u"a CORS-cross-origin script.";
      }
    } else {
      message = base::StrCat(
          {u"\"", module_request.GetModuleTypeString().value_or(u""),
           u"\" is not a valid module type."});
    }
    std::ignore = resolver->Reject(context, TypeError(isolate, message));
    return;
  }
  // Step 4.4: the descendant script fetch options of the calling script's,
  // whose integrity metadata is the import map's for the URL, with an auto
  // fetchpriority that blocks nothing.
  ScriptFetchOptions options;
  options.nonce = referrer_info.Nonce();
  options.integrity_metadata = modulator_->GetIntegrityMetadata(url);
  options.integrity_attribute = modulator_->GetIntegrityMetadataString(url);
  options.parser_state = referrer_info.ParserState();
  options.credentials_mode = referrer_info.CredentialsMode();
  options.referrer_policy = referrer_info.GetReferrerPolicy();
  // Fetch an import() module script graph, whose referrer is the base URL
  // of the script that calls it, which is none for a script without one, as
  // Blink's empty referrer string sends none.
  auto* const tree_client =
      bindings::MakeGarbageCollected<DynamicImportTreeClient>(url, *modulator_,
                                                              resolver);
  modulator_->FetchTree(url, module_type, options, tree_client,
                        referrer_info.BaseUrl().is_empty()
                            ? std::string(kNoReferrer)
                            : referrer_info.BaseUrl().spec());
}

}  // namespace mitto
