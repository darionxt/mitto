// A window's module map, as Blink's ModuleMap keeps it: an entry for each URL
// and module type, "fetching" while its loader runs, and then its module
// script. Each client of an entry hears of its module script from a task of
// its own. A fetch that failed leaves no entry, so that a later import
// fetches again, as ModuleMapDoNotCacheFailedFetch, on for stable, has it.
//
// Upstream: third_party/blink/renderer/core/script/module_map.cc

#ifndef MITTO_DOM_MODULE_MAP_H_
#define MITTO_DOM_MODULE_MAP_H_

#include <map>
#include <string>
#include <utility>

#include "mitto/dom/heap_vector.h"
#include "mitto/dom/modulator.h"
#include "mitto/dom/module_script.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class ModuleMap final : public cppgc::GarbageCollected<ModuleMap> {
 public:
  class Entry;

  explicit ModuleMap(Modulator& modulator);
  ~ModuleMap();

  void Trace(cppgc::Visitor* visitor) const;

  Modulator* GetModulator() const { return modulator_.Get(); }

  // Fetch a single module script: from its entry, or with a new loader.
  void FetchSingleModuleScript(const ModuleScriptFetchRequest& request,
                               ModuleGraphLevel level,
                               SingleModuleClient* client);
  ModuleScript* GetFetchedModuleScript(const GURL& url, ModuleType type) const;

 private:
  using Key = std::pair<std::string, ModuleType>;

  Entry* Find(const Key& key) const;
  // Takes `entry` out of the map, as for a fetch that failed.
  void RemoveEntry(Entry* entry);

  cppgc::Member<Modulator> modulator_;
  // The entries, and where each is by its key.
  HeapVector<Entry> entries_;
  std::map<Key, size_t> index_;
};

}  // namespace mitto

#endif  // MITTO_DOM_MODULE_MAP_H_
