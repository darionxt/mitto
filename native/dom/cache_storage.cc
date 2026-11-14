#include "mitto/dom/cache_storage.h"

#include <utility>
#include <variant>
#include <vector>

#include "base/functional/bind.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_cache_storage.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/cache_storage_messages.h"
#include "mitto/dom/cache.h"
#include "mitto/dom/cache_storage_answers.h"
#include "mitto/dom/cache_storage_host.h"
#include "mitto/dom/document.h"
#include "mitto/dom/request.h"
#include "mitto/dom/response.h"
#include "mitto/dom/window.h"
#include "url/gurl.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {
namespace {

// The Request of `input`, or null after throwing why there is none, as
// Request::Create() of a USVString does with the call's exception state.
Request* RequestOf(const bindings::RequestOrUSVString& input,
                   bindings::ExceptionState& exception_state) {
  if (std::holds_alternative<Request*>(input)) {
    return std::get<Request*>(input);
  }
  return Request::Create(input, bindings::RequestInit(), exception_state);
}

}  // namespace

CacheStorage::CacheStorage(Window* window) : window_(window) {}

CacheStorage::~CacheStorage() = default;

const bindings::WrapperTypeInfo* CacheStorage::GetWrapperTypeInfo() const {
  return &bindings::V8CacheStorage::kWrapperTypeInfo;
}

void CacheStorage::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
}

CacheStorageHost* CacheStorage::Host() const {
  Window* const window = window_.Get();
  if (!window || !window->HasContext()) {
    return nullptr;
  }
  return window->GetDocument().GetCacheStorageHost();
}

v8::Local<v8::Promise> CacheStorage::Ask(PageCacheStorageCall call,
                                         std::string_view method,
                                         Settle settle) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  CacheStorageHost* const host = Host();
  if (!host) {
    // The context was destroyed, and with it the connection.
    std::ignore = resolver->Reject(
        context, CacheCallException(isolate, "InvalidStateError", "", method,
                                    "CacheStorage"));
    return resolver->GetPromise();
  }
  host->Ask(
      std::move(call),
      base::BindOnce(
          [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
             v8::Global<v8::Promise::Resolver> resolver_handle,
             CacheStorageHost* host, Settle settle,
             PageCacheStorageAnswer answer) {
            RunInCacheContext(
                isolate, context_handle, [&](v8::Local<v8::Context> context) {
                  settle(isolate, context, resolver_handle.Get(isolate), *host,
                         Window::Current(), answer);
                });
          },
          isolate, v8::Global<v8::Context>(isolate, context),
          v8::Global<v8::Promise::Resolver>(isolate, resolver), host, settle));
  return resolver->GetPromise();
}

v8::Local<v8::Promise> CacheStorage::Match(
    const bindings::RequestOrUSVString& request,
    const bindings::MultiCacheQueryOptions& options,
    bindings::ExceptionState& exception_state) {
  Request* const request_object = RequestOf(request, exception_state);
  if (!request_object) {
    return {};
  }
  // MatchImpl(): a method other than GET matches nothing, before anything
  // is asked.
  if (request_object->Method() != "GET" && !options.ignore_method) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(isolate->GetCurrentContext())
             .ToLocal(&resolver)) {
      return {};
    }
    std::ignore = resolver->Resolve(isolate->GetCurrentContext(),
                                    v8::Undefined(isolate));
    return resolver->GetPromise();
  }
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kMatch;
  call.request = ToPageCacheRequest(*request_object);
  call.options.ignore_search = options.ignore_search;
  call.options.ignore_method = options.ignore_method;
  call.options.ignore_vary = options.ignore_vary;
  if (options.cache_name) {
    call.has_cache_name = true;
    call.cache_name = *options.cache_name;
  }
  return Ask(std::move(call), "match", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host, Window& window,
                                 const PageCacheStorageAnswer& answer) {
    // MatchComplete(): no entry, an error of storage or no such cache gives
    // undefined.
    if (answer.error == kCacheStorageErrorNotFound ||
        answer.error == kCacheStorageErrorStorage ||
        answer.error == kCacheStorageErrorCacheNameNotFound) {
      std::ignore = resolver->Resolve(context, v8::Undefined(isolate));
      return;
    }
    if (answer.error != kCacheStorageSuccess || answer.responses.empty()) {
      std::ignore = resolver->Reject(
          context, CacheStorageException(isolate, answer.error, "", "match",
                                         "CacheStorage"));
      return;
    }
    std::ignore = resolver->Resolve(
        context, bindings::ToV8(isolate,
                                ResponseFromCache(host, answer.responses[0])));
  });
}

v8::Local<v8::Promise> CacheStorage::Has(
    const std::u16string& cache_name,
    bindings::ExceptionState& exception_state) {
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kHas;
  call.cache_name = cache_name;
  return Ask(std::move(call), "has", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host, Window& window,
                                 const PageCacheStorageAnswer& answer) {
    // HasComplete().
    if (answer.error == kCacheStorageSuccess) {
      std::ignore = resolver->Resolve(context, v8::True(isolate));
    } else if (answer.error == kCacheStorageErrorNotFound) {
      std::ignore = resolver->Resolve(context, v8::False(isolate));
    } else {
      std::ignore = resolver->Reject(
          context, CacheStorageException(isolate, answer.error, "", "has",
                                         "CacheStorage"));
    }
  });
}

v8::Local<v8::Promise> CacheStorage::Open(
    const std::u16string& cache_name,
    bindings::ExceptionState& exception_state) {
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kOpen;
  call.cache_name = cache_name;
  return Ask(std::move(call), "open", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host, Window& window,
                                 const PageCacheStorageAnswer& answer) {
    // OpenComplete().
    if (answer.error != kCacheStorageSuccess) {
      std::ignore = resolver->Reject(
          context, CacheStorageException(isolate, answer.error, "", "open",
                                         "CacheStorage"));
      return;
    }
    std::ignore = resolver->Resolve(
        context, bindings::ToV8(isolate, bindings::MakeGarbageCollected<Cache>(
                                             &window, &host, answer.cache)));
  });
}

v8::Local<v8::Promise> CacheStorage::Delete(
    const std::u16string& cache_name,
    bindings::ExceptionState& exception_state) {
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kDelete;
  call.cache_name = cache_name;
  return Ask(std::move(call), "delete", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host, Window& window,
                                 const PageCacheStorageAnswer& answer) {
    // DeleteComplete(): an error of storage or no such cache gives false.
    if (answer.error == kCacheStorageSuccess) {
      std::ignore = resolver->Resolve(context, v8::True(isolate));
    } else if (answer.error == kCacheStorageErrorStorage ||
               answer.error == kCacheStorageErrorNotFound) {
      std::ignore = resolver->Resolve(context, v8::False(isolate));
    } else {
      std::ignore = resolver->Reject(
          context, CacheStorageException(isolate, answer.error, "", "delete",
                                         "CacheStorage"));
    }
  });
}

v8::Local<v8::Promise> CacheStorage::Keys(
    bindings::ExceptionState& exception_state) {
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kKeys;
  return Ask(std::move(call), "keys", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host, Window& window,
                                 const PageCacheStorageAnswer& answer) {
    std::vector<v8::Local<v8::Value>> names;
    names.reserve(answer.names.size());
    for (const std::u16string& name : answer.names) {
      names.push_back(bindings::ToV8String(isolate, name));
    }
    std::ignore = resolver->Resolve(
        context, v8::Array::New(isolate, names.data(), names.size()));
  });
}

}  // namespace mitto
