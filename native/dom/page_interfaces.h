// Makes a Page's context: a Window for its global object, the interfaces that
// the Runtime implements, and the window's document.

#ifndef MITTO_DOM_PAGE_INTERFACES_H_
#define MITTO_DOM_PAGE_INTERFACES_H_

#include <string_view>

#include "base/functional/function_ref.h"
#include "v8/include/v8-forward.h"

namespace mitto {

// What the Page does once a new context's global object has its interface
// objects and before what the context decides is installed, as Blink tells
// its inspector and its embedder of a new context there, which add what
// Chrome's renderer adds, such as window.chrome.
using DidCreateScriptContext =
    base::FunctionRef<void(v8::Local<v8::Context> context)>;

// Makes a context whose global object is a Window, made from Window's template
// as Blink makes a window's context, puts the interface objects on it as the
// Page's features expose them, calls `did_create`, installs what the context
// decides, and gives the window a document built from `markup`, about:blank's
// for none. The isolate must have its bindings::PerIsolateData, and the caller
// a handle scope.
v8::Local<v8::Context> NewPageContext(v8::Isolate* isolate,
                                      std::u16string_view markup,
                                      DidCreateScriptContext did_create);

}  // namespace mitto

#endif  // MITTO_DOM_PAGE_INTERFACES_H_
