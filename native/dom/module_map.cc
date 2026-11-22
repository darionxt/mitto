#include "mitto/dom/module_map.h"

#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/module_script_loader.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

// A value of the module map: "fetching" while its loader runs, and then its
// module script, with the clients that wait for it meanwhile.
class ModuleMap::Entry final : public cppgc::GarbageCollected<Entry>,
                               public ModuleScriptLoaderClient {
 public:
  Entry(ModuleMap& map, Key key) : map_(&map), key_(std::move(key)) {}

  void Trace(cppgc::Visitor* visitor) const override {
    visitor->Trace(module_script_);
    visitor->Trace(map_);
    visitor->Trace(loader_);
    clients_.Trace(visitor);
  }

  const Key& key() const { return key_; }

  // Tells `client` of the module script from a task, at once if it has come,
  // or once it does.
  void AddClient(SingleModuleClient* client) {
    if (!is_fetching_) {
      DCHECK(clients_.empty());
      DispatchFinishedNotificationAsync(client);
      return;
    }
    clients_.push_back(client);
  }

  ModuleScript* GetModuleScript() const { return module_script_.Get(); }

  // Keeps the loader while it runs, as Blink's ModuleScriptLoaderRegistry
  // does, since the resource that it waits for holds it weakly. Null for one
  // that has finished.
  void SetLoader(ModuleScriptLoader* loader) { loader_ = loader; }

  // ModuleScriptLoaderClient:
  void NotifyNewSingleModuleFinished(ModuleScript* module_script) override {
    CHECK(is_fetching_);
    module_script_ = module_script;
    is_fetching_ = false;
    loader_ = nullptr;
    // Step 9 of fetch a single module script: a fetch that failed leaves no
    // entry. The clients still hear of it.
    if (!module_script_) {
      map_->RemoveEntry(this);
    }
    for (SingleModuleClient* const client : clients_.ToVector()) {
      DispatchFinishedNotificationAsync(client);
    }
    clients_.EraseIf([](SingleModuleClient*) { return true; });
  }

 private:
  void DispatchFinishedNotificationAsync(SingleModuleClient* client) {
    map_->GetModulator()->PostTask(base::BindOnce(
        [](const cppgc::Persistent<SingleModuleClient>& client,
           const cppgc::Persistent<ModuleScript>& module_script) {
          client->NotifyModuleLoadFinished(module_script.Get());
        },
        cppgc::Persistent<SingleModuleClient>(client),
        cppgc::Persistent<ModuleScript>(module_script_.Get())));
  }

  cppgc::Member<ModuleScript> module_script_;
  cppgc::Member<ModuleMap> map_;
  cppgc::Member<ModuleScriptLoader> loader_;
  const Key key_;
  bool is_fetching_ = true;
  HeapVector<SingleModuleClient> clients_;
};

ModuleMap::ModuleMap(Modulator& modulator) : modulator_(&modulator) {}

ModuleMap::~ModuleMap() = default;

void ModuleMap::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(modulator_);
  entries_.Trace(visitor);
}

ModuleMap::Entry* ModuleMap::Find(const Key& key) const {
  const auto found = index_.find(key);
  return found == index_.end() ? nullptr : entries_[found->second];
}

void ModuleMap::FetchSingleModuleScript(const ModuleScriptFetchRequest& request,
                                        ModuleGraphLevel level,
                                        SingleModuleClient* client) {
  // Step 2: if the module map has an entry, wait for it or take it.
  Key key(request.url.spec(), request.expected_module_type);
  Entry* entry = Find(key);
  if (!entry) {
    // Steps 4 to 9 load a new single module script. The loader can finish
    // at once, and take a failed entry out of the map, so `entry` stays on
    // the stack for AddClient() below.
    entry = bindings::MakeGarbageCollected<Entry>(*this, key);
    index_.emplace(std::move(key), entries_.size());
    entries_.push_back(entry);
    entry->SetLoader(
        ModuleScriptLoader::Fetch(request, level, *modulator_, *entry));
  }
  // Step 3, and step 14: complete with the entry's module script.
  if (client) {
    entry->AddClient(client);
  }
}

ModuleScript* ModuleMap::GetFetchedModuleScript(const GURL& url,
                                                ModuleType type) const {
  const Entry* const entry = Find(Key(url.spec(), type));
  return entry ? entry->GetModuleScript() : nullptr;
}

void ModuleMap::RemoveEntry(Entry* entry) {
  const auto found = index_.find(entry->key());
  CHECK(found != index_.end());
  CHECK_EQ(entries_[found->second], entry);
  entries_.EraseIf([entry](Entry* item) { return item == entry; });
  // The entries after it moved down by one.
  index_.clear();
  for (size_t i = 0; i < entries_.size(); ++i) {
    index_.emplace(entries_[i]->key(), i);
  }
}

}  // namespace mitto
