// A module script that a script element has prepared: its graph, which its
// tree client hears of, until it has been fetched and linked, and then
// running it, as Blink's ModulePendingScript does. It is never the current
// script, and fires load only for a src.
//
// Upstream: third_party/blink/renderer/core/script/module_pending_script.cc

#ifndef MITTO_DOM_MODULE_PENDING_SCRIPT_H_
#define MITTO_DOM_MODULE_PENDING_SCRIPT_H_

#include "mitto/dom/modulator.h"
#include "mitto/dom/pending_script.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class ModulePendingScript;
class ModuleScript;
class ScriptElementBase;

// Hears of the graph, which may come before its pending script is made.
class ModulePendingScriptTreeClient final : public ModuleTreeClient {
 public:
  ModulePendingScriptTreeClient();

  void Trace(cppgc::Visitor* visitor) const override;

  void SetPendingScript(ModulePendingScript* pending_script);
  ModuleScript* GetModuleScript() const { return module_script_.Get(); }

  // ModuleTreeClient:
  void NotifyModuleTreeLoadFinished(ModuleScript* module_script) override;

 private:
  bool finished_ = false;
  cppgc::Member<ModuleScript> module_script_;
  cppgc::Member<ModulePendingScript> pending_script_;
};

class ModulePendingScript final : public PendingScript {
 public:
  ModulePendingScript(ScriptElementBase& element,
                      ModulePendingScriptTreeClient& client,
                      bool is_external);
  ~ModulePendingScript() override;

  void Trace(cppgc::Visitor* visitor) const override;

  // Once the graph has come, or failed.
  void NotifyModuleTreeLoadFinished();

  // PendingScript:
  bool IsExternal() const override;
  bool IsReady() const override;

 private:
  // PendingScript:
  bool HasScript() const override;
  bool IsModule() const override;
  void RunScript() override;

  cppgc::Member<ModulePendingScriptTreeClient> module_tree_client_;
  bool ready_ = false;
  const bool is_external_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MODULE_PENDING_SCRIPT_H_
