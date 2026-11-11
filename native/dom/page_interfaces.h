// Gives a Page's context the interfaces that the Runtime implements.

#ifndef MITTO_DOM_PAGE_INTERFACES_H_
#define MITTO_DOM_PAGE_INTERFACES_H_

#include "v8/include/v8-forward.h"

namespace mitto {

// Puts the interface objects on the global object of `context`, as its Page's
// features expose them. The isolate must have its bindings::PerIsolateData.
void InstallPageInterfaces(v8::Local<v8::Context> context);

}  // namespace mitto

#endif  // MITTO_DOM_PAGE_INTERFACES_H_
