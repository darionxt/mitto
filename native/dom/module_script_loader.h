// Fetching one module script, as Blink's ModuleScriptLoader and
// DocumentModuleScriptFetcher do: a window's as a script resource of its
// document's ResourceFetcher, and a worker's through its Fetcher, which the
// Session sends as a module; the checks of its response, which fail it with
// the console errors that Blink gives; and the module script made of it.
//
// Upstream: third_party/blink/renderer/core/loader/modulescript/module_script_loader.cc
// Upstream: third_party/blink/renderer/core/loader/modulescript/module_script_fetcher.cc
// Upstream: third_party/blink/renderer/core/loader/modulescript/document_module_script_fetcher.cc

#ifndef MITTO_DOM_MODULE_SCRIPT_LOADER_H_
#define MITTO_DOM_MODULE_SCRIPT_LOADER_H_

#include <optional>
#include <string>
#include <vector>

#include "mitto/dom/modulator.h"
#include "mitto/dom/module_script.h"
#include "mitto/dom/resource.h"
#include "mitto/dom/script_fetch_options.h"
#include "url/gurl.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class ExecutionContext;
class Fetcher;
class Window;

// What the module map's entry hears of its loader.
class ModuleScriptLoaderClient : public cppgc::GarbageCollectedMixin {
 public:
  virtual void NotifyNewSingleModuleFinished(ModuleScript* module_script) = 0;
};

// The checks of a fetched module's response, ModuleScriptFetcher::
// WasModuleLoadSuccessful() after the status: what the module turned out to
// be, or nullopt, with the console error of a MIME type that `expected` does
// not take in `message`.
std::optional<ResolvedModuleType> ResolveFetchedModuleType(
    const std::string& headers,
    ModuleType expected,
    std::string* message);

class ModuleScriptLoader final
    : public cppgc::GarbageCollected<ModuleScriptLoader>,
      public ResourceClient {
 public:
  // Fetches `request` for `modulator`'s module map, and tells `client` of
  // the module script, or null, once it ends: at once if the request cannot
  // start, or takes a resource that has ended. Returns the loader while it
  // runs, which the caller keeps until then, as Blink's
  // ModuleScriptLoaderRegistry does, since a resource holds its clients
  // weakly, and null once it has finished.
  static ModuleScriptLoader* Fetch(const ModuleScriptFetchRequest& request,
                                   ModuleGraphLevel level,
                                   Modulator& modulator,
                                   ModuleScriptLoaderClient& client);

  ModuleScriptLoader(Modulator& modulator,
                     const ScriptFetchOptions& options,
                     ModuleScriptLoaderClient& client);
  ~ModuleScriptLoader();

  void Trace(cppgc::Visitor* visitor) const override;

  // ResourceClient: DocumentModuleScriptFetcher::NotifyFinished().
  void NotifyFinished(Resource& resource) override;

 private:
  class FetchClient;
  friend class FetchClient;

  void FetchInternal(const ModuleScriptFetchRequest& request,
                     ModuleGraphLevel level);
  // A window's fetch, a script resource of its document's that a request of
  // the same URL, its fragment aside, takes while the document loads or the
  // fetch runs, its response URL with it.
  void FetchThroughDocument(const ModuleScriptFetchRequest& request,
                            Window& window);
  // The referrer that the request goes with: kNoReferrer, the client's, or
  // the base URL of the script that imports it where the Session can take
  // it.
  std::string ReferrerOf(const ModuleScriptFetchRequest& request,
                         const ExecutionContext& context,
                         const Fetcher& fetcher) const;
  // DocumentModuleScriptFetcher::NotifyFinished(), then the loader's.
  void NotifyFetchFinished(int net_error,
                           int status,
                           const GURL& response_url,
                           const std::string& headers,
                           bool opaque,
                           std::string body);
  void Finish(ModuleScript* module_script);

  cppgc::Member<Modulator> modulator_;
  ScriptFetchOptions options_;
  cppgc::Member<ModuleScriptLoaderClient> client_;
  ModuleType expected_module_type_ = ModuleType::kJavaScriptOrWasm;
  GURL url_;
  bool finished_ = false;
};

}  // namespace mitto

#endif  // MITTO_DOM_MODULE_SCRIPT_LOADER_H_
