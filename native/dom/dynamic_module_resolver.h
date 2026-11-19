// import(), as Blink's DynamicModuleResolver runs it: resolving the specifier
// against the base URL of the script that calls it, or of the window where
// there is none, fetching the graph with that script's fetch options, and
// settling the promise with the module's namespace once its evaluation has,
// or with the error.
//
// Upstream: third_party/blink/renderer/core/script/dynamic_module_resolver.cc

#ifndef MITTO_DOM_DYNAMIC_MODULE_RESOLVER_H_
#define MITTO_DOM_DYNAMIC_MODULE_RESOLVER_H_

#include "mitto/dom/module_request.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-promise.h"

namespace mitto {

class Modulator;
class ReferrerScriptInfo;

class DynamicModuleResolver final
    : public cppgc::GarbageCollected<DynamicModuleResolver> {
 public:
  explicit DynamicModuleResolver(Modulator& modulator);

  void Trace(cppgc::Visitor* visitor) const;

  // HostLoadImportedModule for import(): settles the promise of `resolver`.
  void ResolveDynamically(const ModuleRequest& module_request,
                          const ReferrerScriptInfo& referrer_info,
                          v8::Local<v8::Promise::Resolver> resolver);

 private:
  cppgc::Member<Modulator> modulator_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DYNAMIC_MODULE_RESOLVER_H_
