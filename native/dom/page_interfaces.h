// Makes a Page's contexts: a window's, with a Window for its global object, the
// interfaces that the Runtime implements, and the window's document, and a
// dedicated worker's.

#ifndef MITTO_DOM_PAGE_INTERFACES_H_
#define MITTO_DOM_PAGE_INTERFACES_H_

#include <string>
#include <string_view>

#include "base/functional/function_ref.h"
#include "v8/include/v8-forward.h"

namespace mitto {

// What the Page does once a new context's global object has what Window's
// template gives it and before what the context decides is installed, as Blink
// tells its inspector and its embedder of a new context there, which add what
// Chrome's renderer adds, such as window.chrome.
using DidCreateScriptContext =
    base::FunctionRef<void(v8::Local<v8::Context> context)>;

// Makes a context whose global object is a Window, made from Window's template
// as Blink makes a window's context, calls `did_create`, installs what the
// context decides, the interface objects that it exposes among it, as the
// Page's features expose them, and gives the window a document built from
// `markup`, about:blank's for none. The window is cross-origin isolated if
// `cross_origin_isolated`, which decides what of the context its agent allows,
// such as SharedArrayBuffer. The isolate must have its
// bindings::PerIsolateData, and the caller a handle scope.
v8::Local<v8::Context> NewPageContext(v8::Isolate* isolate,
                                      std::u16string_view markup,
                                      bool cross_origin_isolated,
                                      DidCreateScriptContext did_create);

// Makes a context whose global object is a DedicatedWorkerGlobalScope named
// `name`, made from its template as Blink's WorkerOrWorkletScriptController
// makes a dedicated worker's context, calls `did_create`, and installs what
// the context decides, the interface objects that it exposes among it. The
// worker is cross-origin isolated if `cross_origin_isolated`. The isolate must
// have the bindings::PerIsolateData of a worker, and the caller a handle
// scope.
v8::Local<v8::Context> NewWorkerContext(v8::Isolate* isolate,
                                        std::u16string name,
                                        bool cross_origin_isolated,
                                        DidCreateScriptContext did_create);

}  // namespace mitto

#endif  // MITTO_DOM_PAGE_INTERFACES_H_
