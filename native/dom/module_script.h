// A module script, as Blink's ModuleScript has it: its record, its base URL
// and fetch options, its parse error and its error to rethrow, and running it.
// A JavaScript module script compiles its source text; a JSON or text module
// script is a synthetic module whose default export is the value, as Blink's
// ValueWrapperSyntheticModuleScript makes it. A CSS module script needs a
// CSSStyleSheet, which comes with M9's CSSOM: a Known Gap, which fails as a
// fetch does.
//
// Upstream: third_party/blink/renderer/core/script/module_script.cc
// Upstream: third_party/blink/renderer/core/script/js_module_script.cc
// Upstream: third_party/blink/renderer/core/script/value_wrapper_synthetic_module_script.cc
// Upstream: third_party/blink/renderer/core/loader/modulescript/module_script_creation_params.h
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_script_runner.cc

#ifndef MITTO_DOM_MODULE_SCRIPT_H_
#define MITTO_DOM_MODULE_SCRIPT_H_

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mitto/dom/module_request.h"
#include "mitto/dom/script_fetch_options.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-traced-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class Modulator;

// What a module request asks for: the module map's key beside the URL.
enum class ModuleType {
  kInvalid,
  kJavaScriptOrWasm,
  kJSON,
  kCSS,
  kText,
};

// What a fetched module turned out to be. Wasm module scripts need source
// phase imports, which Chrome 157 has off.
enum class ResolvedModuleType {
  kJavaScript,
  kJSON,
  kCSS,
  kText,
};

// ModuleScriptCreationParams: what a module script is made from.
struct ModuleScriptCreationParams {
  ModuleScriptCreationParams();
  ModuleScriptCreationParams(const ModuleScriptCreationParams&);
  ModuleScriptCreationParams& operator=(const ModuleScriptCreationParams&);
  ~ModuleScriptCreationParams();

  // ModuleTypeToString(): how the console names `type`.
  static const char* ModuleTypeToString(ModuleType type);

  GURL source_url;
  GURL base_url;
  ResolvedModuleType module_type = ResolvedModuleType::kJavaScript;
  std::u16string source_text;
  // The referrer policy of the response, kDefault where it named none.
  FetchReferrerPolicy response_referrer_policy = FetchReferrerPolicy::kDefault;
};

class ModuleScript : public cppgc::GarbageCollected<ModuleScript> {
 public:
  // ScriptEvaluationResult's type, for a module.
  enum class ResultType {
    // Scripting was disabled, or the context had gone.
    kNotRun,
    // It threw before it ran: its error to rethrow.
    kException,
    // It ran, and its value is the promise of its evaluation.
    kSuccess,
    // It was terminated.
    kAborted,
  };
  struct EvaluationResult {
    ResultType type = ResultType::kNotRun;
    // The error for kException, the promise for kSuccess.
    v8::Local<v8::Value> value;
  };

  virtual ~ModuleScript();

  virtual void Trace(cppgc::Visitor* visitor) const;

  Modulator* SettingsObject() const { return settings_object_.Get(); }
  const GURL& SourceUrl() const { return source_url_; }
  const GURL& BaseUrl() const { return base_url_; }
  const ScriptFetchOptions& FetchOptions() const { return fetch_options_; }

  // The record, empty for one with a parse error.
  v8::Local<v8::Module> V8Module() const;
  bool HasEmptyRecord() const { return record_.IsEmpty(); }
  std::vector<ModuleRequest> GetModuleRecordRequests() const;
  // Links the record, and returns what that threw, or an empty value.
  v8::Local<v8::Value> Instantiate() const;

  void SetParseErrorAndClearRecord(v8::Local<v8::Value> error);
  bool HasParseError() const { return !parse_error_.IsEmpty(); }
  v8::Local<v8::Value> CreateParseError() const;
  void SetErrorToRethrow(v8::Local<v8::Value> error);
  bool HasErrorToRethrow() const { return !error_to_rethrow_.IsEmpty(); }
  v8::Local<v8::Value> CreateErrorToRethrow() const;

  // The URL that `specifier` resolves to against the base URL, through the
  // window's import maps, or an invalid one, with why in `failure_reason` if
  // that is not null.
  GURL ResolveModuleSpecifier(std::u16string_view specifier,
                              std::string* failure_reason = nullptr) const;

  // V8ScriptRunner::EvaluateModule() in the current context: runs the module
  // and the modules it imports, with the microtasks of the run once it
  // returns. Unless `rethrow_errors`, an error of the evaluation is reported,
  // once its promise rejects, as an error that nothing caught.
  EvaluationResult RunScriptAndReturnValue(bool rethrow_errors);
  // Runs it as a script element does, reporting its errors.
  void RunScript();

 protected:
  ModuleScript(Modulator* settings_object,
               v8::Local<v8::Module> record,
               const GURL& source_url,
               const GURL& base_url,
               const ScriptFetchOptions& fetch_options);

 private:
  cppgc::Member<Modulator> settings_object_;
  v8::TracedReference<v8::Module> record_;
  v8::TracedReference<v8::Value> parse_error_;
  v8::TracedReference<v8::Value> error_to_rethrow_;
  const GURL source_url_;
  const GURL base_url_;
  const ScriptFetchOptions fetch_options_;
  // What each specifier resolved to, as it resolves the same each time.
  mutable std::map<std::u16string, GURL, std::less<>> specifier_to_url_cache_;
};

class JSModuleScript final : public ModuleScript {
 public:
  // JSModuleScript::Create(): compiles `params`' source text in the current
  // context, which starts at `start_line` and `start_column` of its document,
  // zero-based, and sets its parse error to what that or its requests threw.
  static JSModuleScript* Create(const ModuleScriptCreationParams& params,
                                Modulator* modulator,
                                const ScriptFetchOptions& options,
                                int start_line = 0,
                                int start_column = 0);

  JSModuleScript(Modulator* settings_object,
                 v8::Local<v8::Module> record,
                 const GURL& source_url,
                 const GURL& base_url,
                 const ScriptFetchOptions& fetch_options);
};

class ValueWrapperSyntheticModuleScript final : public ModuleScript {
 public:
  // A JSON module script: the source text parsed as JSON, or its parse
  // error.
  static ValueWrapperSyntheticModuleScript*
  CreateJSONWrapperSyntheticModuleScript(
      const ModuleScriptCreationParams& params,
      Modulator* settings_object);
  // A text module script: the source text as a string.
  static ValueWrapperSyntheticModuleScript*
  CreateTextWrapperSyntheticModuleScript(
      const ModuleScriptCreationParams& params,
      Modulator* settings_object);

  ValueWrapperSyntheticModuleScript(Modulator* settings_object,
                                    v8::Local<v8::Module> record,
                                    const GURL& source_url,
                                    const GURL& base_url,
                                    const ScriptFetchOptions& fetch_options);

 private:
  static ValueWrapperSyntheticModuleScript* CreateWithDefaultExport(
      v8::Local<v8::Value> value,
      Modulator* settings_object,
      const GURL& source_url);
  static ValueWrapperSyntheticModuleScript* CreateWithError(
      Modulator* settings_object,
      const GURL& source_url,
      v8::Local<v8::Value> error);
  // The synthetic module's [[EvaluationSteps]]: the default export is the
  // value.
  static v8::MaybeLocal<v8::Promise> EvaluationSteps(
      v8::Local<v8::Context> context,
      v8::Local<v8::Module> module);
};

}  // namespace mitto

#endif  // MITTO_DOM_MODULE_SCRIPT_H_
