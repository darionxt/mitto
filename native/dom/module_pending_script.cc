#include "mitto/dom/module_pending_script.h"

#include "base/check.h"
#include "mitto/dom/module_script.h"

namespace mitto {

ModulePendingScriptTreeClient::ModulePendingScriptTreeClient() = default;

void ModulePendingScriptTreeClient::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(module_script_);
  visitor->Trace(pending_script_);
  ModuleTreeClient::Trace(visitor);
}

void ModulePendingScriptTreeClient::SetPendingScript(
    ModulePendingScript* pending_script) {
  DCHECK(!pending_script_);
  pending_script_ = pending_script;
  if (finished_) {
    pending_script_->NotifyModuleTreeLoadFinished();
  }
}

void ModulePendingScriptTreeClient::NotifyModuleTreeLoadFinished(
    ModuleScript* module_script) {
  DCHECK(!finished_);
  finished_ = true;
  module_script_ = module_script;
  if (pending_script_) {
    pending_script_->NotifyModuleTreeLoadFinished();
  }
}

ModulePendingScript::ModulePendingScript(ScriptElementBase& element,
                                         ModulePendingScriptTreeClient& client,
                                         bool is_external)
    : PendingScript(element),
      module_tree_client_(&client),
      is_external_(is_external) {
  client.SetPendingScript(this);
}

ModulePendingScript::~ModulePendingScript() = default;

void ModulePendingScript::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(module_tree_client_);
  PendingScript::Trace(visitor);
}

void ModulePendingScript::NotifyModuleTreeLoadFinished() {
  CHECK(!IsReady());
  ready_ = true;
  PendingScriptFinished();
}

bool ModulePendingScript::IsExternal() const {
  return is_external_;
}

bool ModulePendingScript::IsReady() const {
  return ready_;
}

bool ModulePendingScript::IsModule() const {
  return true;
}

bool ModulePendingScript::HasScript() const {
  CHECK(IsReady());
  return module_tree_client_->GetModuleScript() != nullptr;
}

void ModulePendingScript::RunScript() {
  module_tree_client_->GetModuleScript()->RunScript();
}

}  // namespace mitto
