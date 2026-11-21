#include "mitto/dom/modulator.h"

#include <tuple>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/document.h"
#include "mitto/dom/dynamic_module_resolver.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/import_map.h"
#include "mitto/dom/module_map.h"
#include "mitto/dom/module_record.h"
#include "mitto/dom/module_tree_linker.h"
#include "mitto/dom/parsed_specifier.h"
#include "mitto/dom/referrer_script_info.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// FindUrlPrefixes(): each prefix of `specifier` that ends in a slash.
std::vector<std::u16string> FindUrlPrefixes(const std::u16string& specifier) {
  std::vector<std::u16string> result;
  size_t position = specifier.find(u'/');
  while (position != std::u16string::npos) {
    ++position;
    result.push_back(specifier.substr(0, position));
    position = specifier.find(u'/', position);
  }
  return result;
}

// ModuleImportMeta::Resolve: import.meta.resolve(), whose data is the
// module's base URL.
void ImportMetaResolve(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* const isolate = info.GetIsolate();
  std::u16string specifier;
  if (!bindings::FromV8String(isolate, info[0], &specifier)) {
    return;
  }
  Modulator* const modulator = Modulator::From(isolate->GetCurrentContext());
  if (!modulator) {
    return;
  }
  std::u16string base_url;
  bindings::FromV8String(isolate, info.Data(), &base_url);
  std::string failure_reason = "Unknown failure";
  const GURL result = modulator->ResolveModuleSpecifier(
      specifier, GURL(base_url), &failure_reason);
  if (!result.is_valid()) {
    isolate->ThrowException(v8::Exception::TypeError(bindings::ToV8String(
        isolate,
        base::StrCat({u"Failed to resolve module specifier ", specifier, u": ",
                      base::UTF8ToUTF16(failure_reason)}))));
    return;
  }
  info.GetReturnValue().Set(
      bindings::ToV8StringFromUtf8(isolate, result.spec()));
}

}  // namespace

v8::MaybeLocal<v8::Promise> HostImportModuleDynamically(
    v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_attributes) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return v8::MaybeLocal<v8::Promise>();
  }
  Modulator* const modulator = Modulator::From(context);
  // A context that has lost its window, whose promise rejects, though its
  // microtasks may never run.
  if (!modulator) {
    std::ignore = resolver->Reject(
        context, v8::Exception::Error(v8::String::NewFromUtf8Literal(
                     isolate,
                     "Cannot import module from an inactive browsing "
                     "context.")));
    return resolver->GetPromise();
  }
  std::u16string specifier_string;
  bindings::FromV8String(isolate, specifier, &specifier_string);
  GURL referrer_resource_url;
  if (resource_name->IsString()) {
    std::u16string name;
    bindings::FromV8String(isolate, resource_name, &name);
    if (!name.empty()) {
      referrer_resource_url = GURL(name);
    }
  }
  const ModuleRequest module_request(
      std::move(specifier_string),
      ModuleRecord::ToImportAttributes(isolate, import_attributes,
                                       /*has_positions=*/false));
  std::u16string invalid_attribute_key;
  if (module_request.HasInvalidImportAttributeKey(&invalid_attribute_key)) {
    std::ignore = resolver->Reject(
        context, v8::Exception::TypeError(bindings::ToV8String(
                     isolate, base::StrCat({u"Invalid attribute key \"",
                                            invalid_attribute_key, u"\"."}))));
  } else {
    modulator->ResolveDynamically(
        module_request,
        ReferrerScriptInfo::FromV8HostDefinedOptions(
            context, host_defined_options, referrer_resource_url),
        resolver);
  }
  return resolver->GetPromise();
}

void HostInitializeImportMetaObject(v8::Local<v8::Context> context,
                                    v8::Local<v8::Module> module,
                                    v8::Local<v8::Object> meta) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  Modulator* const modulator = Modulator::From(context);
  if (!modulator) {
    return;
  }
  // HostGetImportMetaProperties(): url, the module script's base URL, and
  // resolve, a function of one argument that cannot construct.
  const GURL url = modulator->ImportMetaUrl(module);
  const v8::Local<v8::String> url_value =
      bindings::ToV8StringFromUtf8(isolate, url.possibly_invalid_spec());
  v8::Local<v8::Function> resolve;
  if (!v8::Function::New(context, &ImportMetaResolve, url_value, /*length=*/1,
                         v8::ConstructorBehavior::kThrow)
           .ToLocal(&resolve)) {
    return;
  }
  const v8::Local<v8::String> resolve_key =
      v8::String::NewFromUtf8Literal(isolate, "resolve");
  resolve->SetName(resolve_key);
  std::ignore = meta->CreateDataProperty(
      context, v8::String::NewFromUtf8Literal(isolate, "url"), url_value);
  std::ignore = meta->CreateDataProperty(context, resolve_key, resolve);
}

ModuleScriptFetchRequest::ModuleScriptFetchRequest() = default;

ModuleScriptFetchRequest::ModuleScriptFetchRequest(
    const GURL& url,
    ModuleType expected_module_type,
    FetchDestination destination,
    const ScriptFetchOptions& options,
    std::string referrer)
    : url(url),
      expected_module_type(expected_module_type),
      destination(destination),
      options(options),
      referrer(std::move(referrer)) {}

ModuleScriptFetchRequest::ModuleScriptFetchRequest(
    const ModuleScriptFetchRequest&) = default;
ModuleScriptFetchRequest& ModuleScriptFetchRequest::operator=(
    const ModuleScriptFetchRequest&) = default;
ModuleScriptFetchRequest::~ModuleScriptFetchRequest() = default;

// static
Modulator* Modulator::From(v8::Local<v8::Context> context) {
  ExecutionContext* const execution_context =
      ExecutionContext::From(v8::Isolate::GetCurrent(), context);
  if (!execution_context || !execution_context->HasContext()) {
    return nullptr;
  }
  return execution_context->GetModulator();
}

Modulator::Modulator(ExecutionContext& context)
    : context_(&context),
      map_(bindings::MakeGarbageCollected<ModuleMap>(*this)),
      dynamic_module_resolver_(
          bindings::MakeGarbageCollected<DynamicModuleResolver>(*this)),
      // DocumentModulatorImpl starts with an empty import map.
      import_map_(std::make_unique<ImportMap>()) {}

Modulator::~Modulator() = default;

void Modulator::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(context_);
  visitor->Trace(map_);
  visitor->Trace(dynamic_module_resolver_);
  tree_linkers_.Trace(visitor);
  registered_module_scripts_.Trace(visitor);
}

bool Modulator::HasValidContext() const {
  const ExecutionContext* const context = context_.Get();
  return context && context->HasContext();
}

void Modulator::PostTask(base::OnceClosure task) {
  ExecutionContext* const context = context_.Get();
  if (WindowTasks* const tasks = context ? context->GetTasks() : nullptr) {
    tasks->PostTask(TaskType::kNetworking, std::move(task));
  }
}

void Modulator::FetchTree(const GURL& url,
                          ModuleType module_type,
                          const ScriptFetchOptions& options,
                          ModuleTreeClient* client,
                          std::string referrer,
                          FetchDestination destination) {
  ModuleTreeLinker* const linker =
      bindings::MakeGarbageCollected<ModuleTreeLinker>(*this, *client,
                                                       destination);
  AddLinker(linker);
  linker->FetchRoot(url, module_type, options, std::move(referrer));
}

void Modulator::FetchWorkerScriptTree(
    const GURL& url,
    std::shared_ptr<const ModuleWorkerMainScript> main_script,
    const ScriptFetchOptions& options,
    ModuleTreeClient* client) {
  ModuleTreeLinker* const linker =
      bindings::MakeGarbageCollected<ModuleTreeLinker>(
          *this, *client, FetchDestination::kWorker);
  AddLinker(linker);
  // Keyed in the module map by the URL asked for; only the module script's
  // source and base URL are the response's.
  linker->FetchRoot(url, ModuleType::kJavaScriptOrWasm, options,
                    kClientReferrer, std::move(main_script));
}

void Modulator::FetchDescendantsForInlineScript(ModuleScript* module_script,
                                                ModuleTreeClient* client) {
  ModuleTreeLinker* const linker =
      bindings::MakeGarbageCollected<ModuleTreeLinker>(
          *this, *client, FetchDestination::kScript);
  AddLinker(linker);
  linker->FetchRootInline(module_script);
}

void Modulator::FetchSingle(const ModuleScriptFetchRequest& request,
                            ModuleGraphLevel level,
                            SingleModuleClient* client) {
  map_->FetchSingleModuleScript(request, level, client);
}

ModuleScript* Modulator::GetFetchedModuleScript(const GURL& url,
                                                ModuleType type) const {
  return map_->GetFetchedModuleScript(url, type);
}

GURL Modulator::ResolveModuleSpecifier(std::u16string_view specifier,
                                       const GURL& base_url,
                                       std::string* failure_reason) {
  const ParsedSpecifier parsed_specifier =
      ParsedSpecifier::Create(specifier, base_url);
  if (!parsed_specifier.IsValid()) {
    if (failure_reason) {
      *failure_reason =
          "Invalid relative url or base scheme isn't hierarchical.";
    }
    return GURL();
  }
  std::optional<GURL> result;
  if (import_map_) {
    std::string debug_message;
    const std::optional<GURL> mapped_url =
        import_map_->Resolve(parsed_specifier, base_url, &debug_message);
    if (mapped_url) {
      if (!mapped_url->is_valid()) {
        if (failure_reason) {
          *failure_reason = debug_message;
        }
        result = GURL();
      } else {
        result = *mapped_url;
      }
    }
  }
  // Not mapped by the import map.
  if (!result) {
    switch (parsed_specifier.GetType()) {
      case ParsedSpecifier::Type::kInvalid:
        NOTREACHED();
      case ParsedSpecifier::Type::kBare:
        if (failure_reason) {
          *failure_reason =
              "Relative references must start with either \"/\", \"./\", or "
              "\"../\".";
        }
        return GURL();
      case ParsedSpecifier::Type::kURL:
        result = parsed_specifier.GetUrl();
        break;
    }
  }
  // Step 13: add module to resolved module set, then return the result.
  AddModuleToResolvedModuleSet(
      base::UTF8ToUTF16(base_url.possibly_invalid_spec()),
      parsed_specifier.GetImportMapKeyString());
  return *std::move(result);
}

void Modulator::ResolveDynamically(const ModuleRequest& module_request,
                                   const ReferrerScriptInfo& referrer_info,
                                   v8::Local<v8::Promise::Resolver> resolver) {
  // A context that has gone, or a window that has lost its document.
  ExecutionContext* const context = context_.Get();
  if (!context || !context->HasContext() ||
      (context->IsWindow() && !static_cast<Window*>(context)->HasDocument())) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    std::ignore = resolver->Reject(
        isolate->GetCurrentContext(),
        v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(isolate, "No execution context")));
    return;
  }
  dynamic_module_resolver_->ResolveDynamically(module_request, referrer_info,
                                               resolver);
}

ModuleType Modulator::ModuleTypeFromRequest(
    const ModuleRequest& module_request) const {
  const std::optional<std::u16string> module_type_string =
      module_request.GetModuleTypeString();
  if (!module_type_string) {
    return ModuleType::kJavaScriptOrWasm;
  }
  if (*module_type_string == u"json") {
    return ModuleType::kJSON;
  }
  // Only a window's.
  if (*module_type_string == u"css" && context_ && context_->IsWindow()) {
    return ModuleType::kCSS;
  }
  // JavaScriptImportText, stable in Chrome 157.
  if (*module_type_string == u"text") {
    return ModuleType::kText;
  }
  return ModuleType::kInvalid;
}

void Modulator::MergeExistingAndNewImportMaps(
    std::unique_ptr<ImportMap> new_import_map) {
  // Blink drops a warning of a merge that the console already holds.
  import_map_->MergeExistingAndNewImportMaps(
      std::move(new_import_map), scoped_resolved_module_map_,
      toplevel_resolved_module_set_, [this](std::string message) {
        ExecutionContext* const context = context_.Get();
        if (context && merge_warnings_.insert(message).second) {
          context->AddConsoleMessage(ConsoleLevel::kWarning,
                                     std::move(message));
        }
      });
}

std::optional<std::u16string> Modulator::GetIntegrityMetadataString(
    const GURL& url) const {
  if (!import_map_) {
    return std::nullopt;
  }
  return import_map_->ResolveIntegrity(url);
}

IntegrityMetadataSet Modulator::GetIntegrityMetadata(const GURL& url) const {
  const std::optional<std::u16string> value = GetIntegrityMetadataString(url);
  if (!value) {
    return IntegrityMetadataSet();
  }
  std::vector<std::string> messages;
  IntegrityMetadataSet integrity = ParseIntegrityAttribute(*value, &messages);
  if (ExecutionContext* const context = context_.Get()) {
    for (std::string& message : messages) {
      context->AddConsoleMessage(ConsoleLevel::kError, std::move(message));
    }
  }
  return integrity;
}

GURL Modulator::ImportMetaUrl(v8::Local<v8::Module> record) const {
  const ModuleScript* const module_script =
      GetModuleScriptFromModuleRecord(record);
  DCHECK(module_script);
  return module_script->BaseUrl();
}

void Modulator::RegisterModuleScript(ModuleScript* module_script) {
  DCHECK(module_script);
  if (module_script->HasEmptyRecord()) {
    return;
  }
  const v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
  record_index_.emplace(module_script->V8Module()->GetIdentityHash(),
                        registered_module_scripts_.size());
  registered_module_scripts_.push_back(module_script);
}

const ModuleScript* Modulator::GetModuleScriptFromModuleRecord(
    v8::Local<v8::Module> record) const {
  const auto [begin, end] =
      record_index_.equal_range(record->GetIdentityHash());
  for (auto it = begin; it != end; ++it) {
    const ModuleScript* const module_script =
        registered_module_scripts_[it->second];
    const v8::Local<v8::Module> registered = module_script->V8Module();
    if (!registered.IsEmpty() && registered == record) {
      return module_script;
    }
  }
  NOTREACHED() << "no module script for the record's [[HostDefined]]";
}

v8::MaybeLocal<v8::Module> Modulator::ResolveRecord(
    const ModuleRequest& module_request,
    v8::Local<v8::Module> referrer) {
  // Steps 3 to 7: the referrer's module script resolves the specifier, and
  // the module map has the result.
  const ModuleScript* const referrer_module =
      GetModuleScriptFromModuleRecord(referrer);
  const GURL url =
      referrer_module->ResolveModuleSpecifier(module_request.specifier);
  const ModuleType child_module_type = ModuleTypeFromRequest(module_request);
  DCHECK(url.is_valid());
  CHECK(child_module_type != ModuleType::kInvalid);
  ModuleScript* const module_script =
      GetFetchedModuleScript(url, child_module_type);
  // Steps 8 to 10: it is a module script with a record.
  DCHECK(module_script);
  const v8::Local<v8::Module> record = module_script->V8Module();
  CHECK(!record.IsEmpty());
  return record;
}

void Modulator::AddLinker(ModuleTreeLinker* linker) {
  tree_linkers_.push_back(linker);
}

void Modulator::ReleaseFinishedLinker(ModuleTreeLinker* linker) {
  tree_linkers_.EraseIf(
      [linker](ModuleTreeLinker* item) { return item == linker; });
}

void Modulator::AddModuleToResolvedModuleSet(
    const std::u16string& referring_script_url,
    const std::u16string& specifier) {
  // DocumentModulatorImpl keeps each prefix of the specifier, at the top
  // level and for each prefix of the referring script's URL, and that URL
  // itself, which a scope may name exactly.
  const std::vector<std::u16string> specifier_prefixes =
      FindUrlPrefixes(specifier);
  toplevel_resolved_module_set_.insert(specifier);
  toplevel_resolved_module_set_.insert(specifier_prefixes.begin(),
                                       specifier_prefixes.end());
  if (referring_script_url.empty()) {
    return;
  }
  std::vector<std::u16string> referring_script_prefixes =
      FindUrlPrefixes(referring_script_url);
  referring_script_prefixes.push_back(referring_script_url);
  for (const std::u16string& prefix : referring_script_prefixes) {
    std::set<std::u16string>& current_set = scoped_resolved_module_map_[prefix];
    current_set.insert(specifier);
    current_set.insert(specifier_prefixes.begin(), specifier_prefixes.end());
  }
}

}  // namespace mitto
