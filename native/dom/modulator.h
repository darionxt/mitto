// A window's module machinery, as Blink's Modulator, ModulatorImplBase and
// DocumentModulatorImpl have it for a document: its module map, which keeps
// each module script once fetched, keyed by URL and module type; resolving a
// module specifier, through the window's import maps; fetching a module graph
// and an import()'s; import.meta; and, as Blink's ModuleRecordResolverImpl,
// which module script a V8 module record is, for linking.
//
// Upstream: third_party/blink/renderer/core/script/modulator.h
// Upstream: third_party/blink/renderer/core/script/modulator_impl_base.cc
// Upstream: third_party/blink/renderer/core/script/document_modulator_impl.cc
// Upstream: third_party/blink/renderer/core/script/module_record_resolver_impl.cc
// Upstream: third_party/blink/renderer/core/loader/modulescript/module_script_fetch_request.h
// Upstream: third_party/blink/renderer/core/loader/modulescript/module_tree_linker_registry.cc

#ifndef MITTO_DOM_MODULATOR_H_
#define MITTO_DOM_MODULATOR_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/functional/callback.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/module_request.h"
#include "mitto/dom/module_script.h"
#include "mitto/dom/script_fetch_options.h"
#include "mitto/dom/subresource_integrity.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-data.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"

namespace mitto {

class DynamicModuleResolver;
class ImportMap;
class ModuleMap;
class ModuleTreeLinker;
class ReferrerScriptInfo;
class Window;

// Hears that one module script has been fetched, or null for one that
// failed.
class SingleModuleClient : public cppgc::GarbageCollected<SingleModuleClient> {
 public:
  virtual ~SingleModuleClient() = default;
  virtual void Trace(cppgc::Visitor* visitor) const {}
  virtual void NotifyModuleLoadFinished(ModuleScript* module_script) = 0;
};

// Hears that a module graph has been fetched and linked, or null for one
// that failed.
class ModuleTreeClient : public cppgc::GarbageCollected<ModuleTreeClient> {
 public:
  virtual ~ModuleTreeClient() = default;
  virtual void Trace(cppgc::Visitor* visitor) const {}
  virtual void NotifyModuleTreeLoadFinished(ModuleScript* module_script) = 0;
};

enum class ModuleGraphLevel {
  kTopLevelModuleFetch,
  kDependentModuleFetch,
};

// What to fetch one module script with.
struct ModuleScriptFetchRequest {
  ModuleScriptFetchRequest();
  ModuleScriptFetchRequest(const GURL& url,
                           ModuleType expected_module_type,
                           const ScriptFetchOptions& options,
                           std::string referrer);
  ModuleScriptFetchRequest(const ModuleScriptFetchRequest&);
  ModuleScriptFetchRequest& operator=(const ModuleScriptFetchRequest&);
  ~ModuleScriptFetchRequest();

  GURL url;
  ModuleType expected_module_type = ModuleType::kJavaScriptOrWasm;
  ScriptFetchOptions options;
  // kClientReferrer, or the URL of the script that imports it.
  std::string referrer = kClientReferrer;
};

class Modulator final : public cppgc::GarbageCollected<Modulator> {
 public:
  // Whether the document still acquires import maps, which it does until its
  // first module script or import() starts.
  enum class AcquiringImportMapsState {
    kAcquiring,
    kAfterModuleScriptLoad,
  };

  // The modulator of the window of `context`, or null once the window's
  // context has gone, as Blink's is the context's data.
  static Modulator* From(v8::Local<v8::Context> context);

  explicit Modulator(Window& window);
  ~Modulator();

  void Trace(cppgc::Visitor* visitor) const;

  Window* GetWindow() const { return window_.Get(); }
  // Whether the window still has its context.
  bool HasValidContext() const;
  // Runs `task` from a task of the networking task source, in the window's
  // context.
  void PostTask(base::OnceClosure task);

  // Fetch a module script graph, from `url`, as `module_type`, with
  // `options`, as import() and a script element's module do.
  void FetchTree(const GURL& url,
                 ModuleType module_type,
                 const ScriptFetchOptions& options,
                 ModuleTreeClient* client,
                 std::string referrer = kClientReferrer);
  // Fetch the descendants of an inline module script, and link it.
  void FetchDescendantsForInlineScript(ModuleScript* module_script,
                                       ModuleTreeClient* client);
  // Fetch a single module script, from the module map if it has it.
  void FetchSingle(const ModuleScriptFetchRequest& request,
                   ModuleGraphLevel level,
                   SingleModuleClient* client);
  // The module script that the module map holds for `url` and `type`, null
  // where it has none or is still fetching it.
  ModuleScript* GetFetchedModuleScript(const GURL& url, ModuleType type) const;

  // Resolve a module specifier: `specifier` against `base_url`, through the
  // import map, or an invalid URL, with why in `failure_reason` if that is
  // not null.
  GURL ResolveModuleSpecifier(std::u16string_view specifier,
                              const GURL& base_url,
                              std::string* failure_reason);
  // HostLoadImportedModule for import(): settles the promise of `resolver`.
  void ResolveDynamically(const ModuleRequest& module_request,
                          const ReferrerScriptInfo& referrer_info,
                          v8::Local<v8::Promise::Resolver> resolver);
  // The module type that `module_request` asks for, kInvalid for one not
  // allowed.
  ModuleType ModuleTypeFromRequest(const ModuleRequest& module_request) const;

  // Import maps.
  //
  // Merge existing and new import maps.
  void MergeExistingAndNewImportMaps(std::unique_ptr<ImportMap> new_import_map);
  AcquiringImportMapsState GetAcquiringImportMapsState() const {
    return acquiring_import_maps_;
  }
  void SetAcquiringImportMapsState(AcquiringImportMapsState state) {
    acquiring_import_maps_ = state;
  }
  // Resolving a module integrity metadata: what the import map's integrity
  // says of `url`, null where it says nothing.
  std::optional<std::u16string> GetIntegrityMetadataString(
      const GURL& url) const;
  IntegrityMetadataSet GetIntegrityMetadata(const GURL& url) const;

  // HostGetImportMetaProperties(): import.meta.url of `record`.
  GURL ImportMetaUrl(v8::Local<v8::Module> record) const;

  // ModuleRecordResolverImpl: the module script whose record is `record`,
  // which the module script registers once it has compiled.
  void RegisterModuleScript(ModuleScript* module_script);
  const ModuleScript* GetModuleScriptFromModuleRecord(
      v8::Local<v8::Module> record) const;
  // HostLoadImportedModule for linking: the record of what `referrer`
  // imports as `module_request`, which the module map holds.
  v8::MaybeLocal<v8::Module> ResolveRecord(const ModuleRequest& module_request,
                                           v8::Local<v8::Module> referrer);

  // ModuleTreeLinkerRegistry: the linkers that run, which this keeps until
  // they finish.
  void ReleaseFinishedLinker(ModuleTreeLinker* linker);

 private:
  // Add module to resolved module set.
  void AddModuleToResolvedModuleSet(const std::u16string& referring_script_url,
                                    const std::u16string& specifier);
  void AddLinker(ModuleTreeLinker* linker);

  cppgc::WeakMember<Window> window_;
  cppgc::Member<ModuleMap> map_;
  cppgc::Member<DynamicModuleResolver> dynamic_module_resolver_;
  HeapVector<ModuleTreeLinker> tree_linkers_;
  // The module scripts with a record, and where each is among them by the
  // identity hash of its record, as Blink's map of boxed records.
  HeapVector<ModuleScript> registered_module_scripts_;
  std::unordered_multimap<int, size_t> record_index_;
  std::unique_ptr<ImportMap> import_map_;
  AcquiringImportMapsState acquiring_import_maps_ =
      AcquiringImportMapsState::kAcquiring;
  // The resolved module set, as DocumentModulatorImpl keeps it: each
  // specifier and its prefixes, at the top level and for each prefix of the
  // URL of the script that resolved it.
  std::map<std::u16string, std::set<std::u16string>>
      scoped_resolved_module_map_;
  std::set<std::u16string> toplevel_resolved_module_set_;
  // The warnings that merging import maps has given.
  std::set<std::string> merge_warnings_;
};

// V8's callbacks for import() and import.meta, which the Page's isolate
// sets, as Blink's V8Initializer does.
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_initializer.cc
// Upstream: third_party/blink/renderer/core/script/module_import_meta.cc
v8::MaybeLocal<v8::Promise> HostImportModuleDynamically(
    v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_attributes);
void HostInitializeImportMetaObject(v8::Local<v8::Context> context,
                                    v8::Local<v8::Module> module,
                                    v8::Local<v8::Object> meta);

}  // namespace mitto

#endif  // MITTO_DOM_MODULATOR_H_
