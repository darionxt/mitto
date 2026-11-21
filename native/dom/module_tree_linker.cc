#include "mitto/dom/module_tree_linker.h"

#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

struct ModuleScriptFetchTarget {
  GURL url;
  ModuleType module_type;
};

}  // namespace

ModuleTreeLinker::ModuleTreeLinker(Modulator& modulator,
                                   ModuleTreeClient& client,
                                   FetchDestination destination)
    : modulator_(&modulator), client_(&client), destination_(destination) {}

ModuleTreeLinker::~ModuleTreeLinker() = default;

void ModuleTreeLinker::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(modulator_);
  visitor->Trace(client_);
  visitor->Trace(result_);
  SingleModuleClient::Trace(visitor);
}

void ModuleTreeLinker::AdvanceState(State new_state) {
  switch (state_) {
    case State::kInitial:
      CHECK_EQ(num_incomplete_fetches_, 0u);
      CHECK(new_state == State::kFetchingSelf);
      break;
    case State::kFetchingSelf:
      CHECK_EQ(num_incomplete_fetches_, 0u);
      CHECK(new_state == State::kFetchingDependencies ||
            new_state == State::kFinished);
      break;
    case State::kFetchingDependencies:
      CHECK(new_state == State::kInstantiating ||
            new_state == State::kFinished);
      break;
    case State::kInstantiating:
      CHECK(new_state == State::kFinished);
      break;
    case State::kFinished:
      NOTREACHED();
  }
  state_ = new_state;
  if (state_ == State::kFinished) {
    // The registry lets the linker go, and the algorithm completes with its
    // result.
    modulator_->ReleaseFinishedLinker(this);
    client_->NotifyModuleTreeLoadFinished(result_.Get());
  }
}

void ModuleTreeLinker::FetchRoot(
    const GURL& url,
    ModuleType module_type,
    const ScriptFetchOptions& options,
    std::string referrer,
    std::shared_ptr<const ModuleWorkerMainScript> main_script) {
  // Wait for import maps: the document no longer acquires them.
  modulator_->SetAcquiringImportMapsState(
      Modulator::AcquiringImportMapsState::kAfterModuleScriptLoad);
  AdvanceState(State::kFetchingSelf);
  // A URL that is null completes with null, from a task.
  if (!url.is_valid()) {
    result_ = nullptr;
    modulator_->PostTask(base::BindOnce(
        [](const cppgc::Persistent<ModuleTreeLinker>& linker) {
          linker->AdvanceState(State::kFinished);
        },
        cppgc::Persistent<ModuleTreeLinker>(this)));
    return;
  }
  CHECK(module_type != ModuleType::kInvalid);
  visited_set_.emplace(url.spec(), module_type);
  ++num_incomplete_fetches_;
  // Fetch a single module script, with the top-level module fetch flag set,
  // and the referrer that the caller gives: the client, or for import() the
  // URL of the script that calls it.
  ModuleScriptFetchRequest request(url, module_type, destination_, options,
                                   std::move(referrer));
  request.main_script = std::move(main_script);
  modulator_->FetchSingle(request, ModuleGraphLevel::kTopLevelModuleFetch,
                          this);
}

void ModuleTreeLinker::FetchRootInline(ModuleScript* module_script) {
  DCHECK(module_script);
  modulator_->SetAcquiringImportMapsState(
      Modulator::AcquiringImportMapsState::kAfterModuleScriptLoad);
  AdvanceState(State::kFetchingSelf);
  // The script is the result once the descendants have come.
  result_ = module_script;
  AdvanceState(State::kFetchingDependencies);
  modulator_->PostTask(base::BindOnce(
      [](const cppgc::Persistent<ModuleTreeLinker>& linker,
         const cppgc::Persistent<ModuleScript>& module_script) {
        linker->FetchDescendants(module_script.Get());
      },
      cppgc::Persistent<ModuleTreeLinker>(this),
      cppgc::Persistent<ModuleScript>(module_script)));
}

void ModuleTreeLinker::NotifyModuleLoadFinished(ModuleScript* module_script) {
  CHECK_GT(num_incomplete_fetches_, 0u);
  --num_incomplete_fetches_;
  if (state_ == State::kFetchingSelf) {
    // The top-level module, which is linked and returned later.
    result_ = module_script;
    AdvanceState(State::kFetchingDependencies);
  }
  // A descendant failed meanwhile, while others were still fetching.
  if (state_ != State::kFetchingDependencies) {
    return;
  }
  if (!module_script) {
    result_ = nullptr;
    AdvanceState(State::kFinished);
    return;
  }
  FetchDescendants(module_script);
}

void ModuleTreeLinker::FetchDescendants(const ModuleScript* module_script) {
  DCHECK(module_script);
  if (AbortBeforeFinalizingIfNecessary(module_script)) {
    return;
  }
  // Steps 4 and 5: the requests of the record whose URL and module type are
  // not in the visited set yet.
  std::vector<ModuleScriptFetchTarget> module_requests;
  for (const ModuleRequest& requested :
       module_script->GetModuleRecordRequests()) {
    const GURL url = module_script->ResolveModuleSpecifier(requested.specifier);
    const ModuleType module_type = modulator_->ModuleTypeFromRequest(requested);
    // Resolving succeeded once, when the record was made.
    CHECK(url.is_valid());
    CHECK(module_type != ModuleType::kInvalid);
    if (visited_set_.emplace(url.spec(), module_type).second) {
      module_requests.push_back({url, module_type});
    }
  }
  if (module_requests.empty()) {
    FinalizeFetchDescendantsForOneModuleScript();
    return;
  }
  // Step 8: the internal module script graph fetching procedure for each, in
  // parallel, with the descendant script fetch options: the same but for the
  // integrity metadata, the import map's for the URL, and an auto
  // fetchpriority that blocks nothing.
  for (const ModuleScriptFetchTarget& target : module_requests) {
    ScriptFetchOptions options = module_script->FetchOptions();
    options.integrity_metadata = modulator_->GetIntegrityMetadata(target.url);
    options.integrity_attribute =
        modulator_->GetIntegrityMetadataString(target.url);
    options.fetch_priority_hint = FetchPriorityHint::kAuto;
    options.render_blocking = RenderBlockingBehavior::kNonBlocking;
    ++num_incomplete_fetches_;
    modulator_->FetchSingle(
        ModuleScriptFetchRequest(target.url, target.module_type, destination_,
                                 options, module_script->BaseUrl().spec()),
        ModuleGraphLevel::kDependentModuleFetch, this);
  }
  CHECK_GT(num_incomplete_fetches_, 0u);
}

void ModuleTreeLinker::FinalizeFetchDescendantsForOneModuleScript() {
  if (num_incomplete_fetches_ == 0) {
    Instantiate();
  }
}

bool ModuleTreeLinker::AbortBeforeFinalizingIfNecessary(
    const ModuleScript* module_script) {
  if (!modulator_->HasValidContext()) {
    result_ = nullptr;
    AdvanceState(State::kFinished);
    return true;
  }
  // Step 1: a module script with no record completes as it is. The rest of
  // the graph still comes, since which parse error counts is not known yet.
  if (module_script->HasEmptyRecord()) {
    found_parse_error_ = true;
    FinalizeFetchDescendantsForOneModuleScript();
    return true;
  }
  return false;
}

void ModuleTreeLinker::Instantiate() {
  if (!modulator_->HasValidContext()) {
    result_ = nullptr;
    AdvanceState(State::kFinished);
    return;
  }
  // Step 3.
  if (!result_) {
    AdvanceState(State::kFinished);
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  if (!found_parse_error_) {
    // Step 5.2: link the record, whose error becomes the error to rethrow.
    AdvanceState(State::kInstantiating);
    const v8::Local<v8::Value> instantiation_error = result_->Instantiate();
    if (!instantiation_error.IsEmpty()) {
      result_->SetErrorToRethrow(instantiation_error);
    }
  } else {
    // Step 6: the first parse error is the error to rethrow.
    std::set<const ModuleScript*> discovered_set;
    const v8::Local<v8::Value> parse_error =
        FindFirstParseError(result_.Get(), &discovered_set);
    DCHECK(!parse_error.IsEmpty());
    result_->SetErrorToRethrow(parse_error);
  }
  AdvanceState(State::kFinished);
}

v8::Local<v8::Value> ModuleTreeLinker::FindFirstParseError(
    const ModuleScript* module_script,
    std::set<const ModuleScript*>* discovered_set) const {
  // Only called once every module script of the graph has come.
  DCHECK(module_script);
  discovered_set->insert(module_script);
  if (module_script->HasEmptyRecord()) {
    return module_script->CreateParseError();
  }
  for (const ModuleRequest& requested :
       module_script->GetModuleRecordRequests()) {
    const GURL child_url =
        module_script->ResolveModuleSpecifier(requested.specifier);
    const ModuleType child_module_type =
        modulator_->ModuleTypeFromRequest(requested);
    CHECK(child_url.is_valid());
    CHECK(child_module_type != ModuleType::kInvalid);
    const ModuleScript* const child_module =
        modulator_->GetFetchedModuleScript(child_url, child_module_type);
    CHECK(child_module);
    if (discovered_set->contains(child_module)) {
      continue;
    }
    const v8::Local<v8::Value> child_parse_error =
        FindFirstParseError(child_module, discovered_set);
    if (!child_parse_error.IsEmpty()) {
      return child_parse_error;
    }
  }
  return v8::Local<v8::Value>();
}

}  // namespace mitto
