// Fetching a module graph and linking it, as Blink's ModuleTreeLinker does:
// the root, then the descendants of each module script that it fetches, in
// parallel, each URL and module type once, then the first parse error of the
// graph, or else its instantiation, whose error becomes the root's error to
// rethrow.
//
// Upstream: third_party/blink/renderer/core/loader/modulescript/module_tree_linker.cc

#ifndef MITTO_DOM_MODULE_TREE_LINKER_H_
#define MITTO_DOM_MODULE_TREE_LINKER_H_

#include <set>
#include <string>
#include <utility>

#include "mitto/dom/heap_vector.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/module_script.h"
#include "url/gurl.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-value.h"

namespace mitto {

class ModuleTreeLinker final : public SingleModuleClient {
 public:
  ModuleTreeLinker(Modulator& modulator, ModuleTreeClient& client);
  ~ModuleTreeLinker() override;

  void Trace(cppgc::Visitor* visitor) const override;

  // Fetch a module script tree, an import() module script graph: from the
  // root at `url`.
  void FetchRoot(const GURL& url,
                 ModuleType module_type,
                 const ScriptFetchOptions& options,
                 std::string referrer);
  // Fetch an inline module script graph: from `module_script`, already made.
  void FetchRootInline(ModuleScript* module_script);

  // SingleModuleClient:
  void NotifyModuleLoadFinished(ModuleScript* module_script) override;

 private:
  enum class State {
    kInitial,
    kFetchingSelf,
    kFetchingDependencies,
    kInstantiating,
    kFinished,
  };

  void AdvanceState(State new_state);
  void FetchDescendants(const ModuleScript* module_script);
  void FinalizeFetchDescendantsForOneModuleScript();
  // Whether to stop here, for a context that has gone or a module script
  // with no record.
  bool AbortBeforeFinalizingIfNecessary(const ModuleScript* module_script);
  void Instantiate();
  // Finding the first parse error, which is empty where there is none.
  v8::Local<v8::Value> FindFirstParseError(
      const ModuleScript* module_script,
      std::set<const ModuleScript*>* discovered_set) const;

  cppgc::Member<Modulator> modulator_;
  cppgc::Member<ModuleTreeClient> client_;
  State state_ = State::kInitial;
  cppgc::Member<ModuleScript> result_;
  bool found_parse_error_ = false;
  size_t num_incomplete_fetches_ = 0;
  // The visited set: each URL, serialized, and module type.
  std::set<std::pair<std::string, ModuleType>> visited_set_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MODULE_TREE_LINKER_H_
