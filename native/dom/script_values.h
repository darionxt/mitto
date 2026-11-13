// What the DOM's classes hand a page as Blink makes it: frozen arrays, plain
// objects of a dictionary's members, and promises that a task of their own
// settles.
//
// Upstream: third_party/blink/renderer/bindings/core/v8/to_v8_traits.h
// Upstream: third_party/blink/renderer/bindings/core/v8/v8_object_builder.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/script_promise_resolver.cc
// Upstream: third_party/blink/renderer/bindings/core/v8/callback_invoke_helper.cc

#ifndef MITTO_DOM_SCRIPT_VALUES_H_
#define MITTO_DOM_SCRIPT_VALUES_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"

namespace mitto {

// An array of `items`, frozen, as Blink gives a FrozenArray<T>.
v8::Local<v8::Array> FrozenArray(
    v8::Isolate* isolate,
    const std::vector<v8::Local<v8::Value>>& items);
// The same, of strings.
v8::Local<v8::Array> FrozenArrayOfStrings(
    v8::Isolate* isolate,
    const std::vector<std::u16string>& items);

// An object with each of `members` as a data property, in their order, as
// Blink's generated code writes a dictionary's members and V8ObjectBuilder an
// object.
v8::Local<v8::Object> PlainObject(
    v8::Isolate* isolate,
    const std::vector<std::pair<std::string_view, v8::Local<v8::Value>>>&
        members);

// Resolves `resolver` with `value` from a task of its own, as Blink's
// ScriptPromiseResolver does once an answer comes, unless the window of the
// current context has gone by then.
void ResolveFromTask(v8::Isolate* isolate,
                     v8::Local<v8::Promise::Resolver> resolver,
                     v8::Local<v8::Value> value);

// The same with what `make_value` gives in that task, which may run a page's
// callback first, as Blink's answer to a request does.
void ResolveFromTaskWith(
    v8::Isolate* isolate,
    v8::Local<v8::Promise::Resolver> resolver,
    base::OnceCallback<v8::Local<v8::Value>(v8::Isolate*)> make_value);

// Rejects `resolver` from a task of its own, as ResolveFromTask() resolves,
// with a DOMException of `name` and `message` that the task makes, so that its
// stack holds no script, as Blink's does.
void RejectWithDOMExceptionFromTask(v8::Isolate* isolate,
                                    v8::Local<v8::Promise::Resolver> resolver,
                                    std::string name,
                                    std::string message);

// Calls `callback`, an object of the callback interface `interface_name`, with
// what `make_argument` gives, from a task of its own, as Blink's
// AsyncCallbackHelper does once an answer comes: the object itself if it is
// callable, and otherwise its handleEvent(), in the object's context, unless
// that context's window has gone by then. What the callback queued runs once
// it returns, and what it throws is reported, as V8ScriptRunner does.
void CallCallbackInterfaceFromTask(
    v8::Isolate* isolate,
    v8::Local<v8::Object> callback,
    std::string interface_name,
    base::OnceCallback<v8::Local<v8::Value>(v8::Isolate*)> make_argument);

}  // namespace mitto

#endif  // MITTO_DOM_SCRIPT_VALUES_H_
