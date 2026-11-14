#include "mitto/dom/cache.h"

#include <set>
#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_cache.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/bindings/v8_response.h"
#include "mitto/dom/abort_controller.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/cache_storage_answers.h"
#include "mitto/dom/cache_storage_host.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/request.h"
#include "mitto/dom/response.h"
#include "mitto/dom/window.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"

namespace mitto {
namespace {

Request* RequestOf(const bindings::RequestOrUSVString& input,
                   bindings::ExceptionState& exception_state) {
  if (std::holds_alternative<Request*>(input)) {
    return std::get<Request*>(input);
  }
  return Request::Create(input, bindings::RequestInit(), exception_state);
}

// ValidateRequestForPut(): only a GET of an HTTP or HTTPS URL is put.
bool ValidateRequestForPut(const Request& request,
                           bindings::ExceptionState& exception_state) {
  if (!request.url().SchemeIsHTTPOrHTTPS()) {
    exception_state.ThrowTypeError(base::StrCat(
        {"Request scheme '", request.url().scheme(), "' is unsupported"}));
    return false;
  }
  if (request.Method() != "GET") {
    exception_state.ThrowTypeError(base::StrCat(
        {"Request method '", request.Method(), "' is unsupported"}));
    return false;
  }
  return true;
}

// VaryHeaderContainsAsterisk().
bool VaryHeaderContainsAsterisk(const Response& response) {
  const std::optional<std::string> vary = response.Headers()->GetValue("vary");
  if (!vary) {
    return false;
  }
  for (std::string_view field :
       base::SplitStringPiece(*vary, ",", base::TRIM_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    if (field == "*") {
      return true;
    }
  }
  return false;
}

v8::Local<v8::Promise> SettledPromise(v8::Isolate* isolate,
                                      v8::Local<v8::Value> value) {
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  std::ignore = resolver->Resolve(context, value);
  return resolver->GetPromise();
}

// PopulateFetchAPIResponse(): what the Session stores of a response.
PageCacheResponse ToPageCacheResponse(const Response& response) {
  PageCacheResponse page_response;
  const std::string url = base::UTF16ToUTF8(response.Url());
  // Its URL list: empty for a response that a page made, and with every URL
  // of its redirects for a fetched one, of which only the last and whether
  // there are more than one show.
  if (!url.empty()) {
    page_response.url_list.push_back(url);
    if (response.Redirected()) {
      page_response.url_list.push_back(url);
    }
  }
  page_response.status = response.Status();
  page_response.status_text = response.StatusText();
  const std::u16string type = response.Type();
  page_response.type = type == u"basic"    ? 0
                       : type == u"cors"   ? 1
                       : type == u"opaque" ? 4
                                           : 2;
  // Each name once, the first of its values, as the mojom's map holds them.
  std::set<std::string> seen;
  for (const auto& [name, value] : response.Headers()->list()) {
    std::string lower = base::ToLowerASCII(name);
    if (seen.insert(lower).second) {
      page_response.headers.emplace_back(std::move(lower), value);
    }
  }
  // A CORS response holds only the lines it exposes already, so each of
  // them stays exposed.
  if (page_response.type == 1) {
    for (const auto& [name, value] : page_response.headers) {
      page_response.cors_exposed_header_names.push_back(name);
    }
  }
  page_response.response_time =
      base::Time::Now().ToDeltaSinceWindowsEpoch().InMicroseconds();
  return page_response;
}

}  // namespace

Cache::PutBarrier::PutBarrier() = default;
Cache::PutBarrier::PutBarrier(PutBarrier&&) = default;
Cache::PutBarrier& Cache::PutBarrier::operator=(PutBarrier&&) = default;
Cache::PutBarrier::~PutBarrier() = default;

Cache::Cache(Window* window, CacheStorageHost* host, uint64_t cache)
    : window_(window), host_(host), cache_(cache) {}

Cache::~Cache() = default;

const bindings::WrapperTypeInfo* Cache::GetWrapperTypeInfo() const {
  return &bindings::V8Cache::kWrapperTypeInfo;
}

void Cache::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(window_);
}

void Cache::Dispose() {
  // Garbage collection may not send, so the cache is released from a task.
  EventLoop::Get()
      ->TaskRunner(TaskType::kMiscPlatformAPI)
      ->PostTask(FROM_HERE,
                 base::BindOnce(
                     [](CacheStorageHost* host, uint64_t cache) {
                       host->ReleaseCache(cache);
                     },
                     host_.get(), cache_));
}

CacheStorageHost* Cache::Host() const {
  Window* const window = window_.Get();
  if (!window || !window->HasContext()) {
    return nullptr;
  }
  return host_;
}

v8::Local<v8::Promise> Cache::Ask(PageCacheStorageCall call,
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
    std::ignore = resolver->Reject(
        context,
        CacheCallException(isolate, "InvalidStateError", "", method, "Cache"));
    return resolver->GetPromise();
  }
  call.cache = cache_;
  host->Ask(std::move(call),
            base::BindOnce(
                [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
                   v8::Global<v8::Promise::Resolver> resolver_handle,
                   cppgc::Persistent<Cache> self, Settle settle,
                   PageCacheStorageAnswer answer) {
                  RunInCacheContext(isolate, context_handle,
                                    [&](v8::Local<v8::Context> context) {
                                      settle(isolate, context,
                                             resolver_handle.Get(isolate),
                                             *self->host_, answer);
                                    });
                },
                isolate, v8::Global<v8::Context>(isolate, context),
                v8::Global<v8::Promise::Resolver>(isolate, resolver),
                cppgc::Persistent<Cache>(this), settle));
  return resolver->GetPromise();
}

v8::Local<v8::Promise> Cache::Match(const bindings::RequestOrUSVString& request,
                                    const bindings::CacheQueryOptions& options,
                                    bindings::ExceptionState& exception_state) {
  Request* const request_object = RequestOf(request, exception_state);
  if (!request_object) {
    return {};
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (request_object->Method() != "GET" && !options.ignore_method) {
    return SettledPromise(isolate, v8::Undefined(isolate));
  }
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kCacheMatch;
  call.request = ToPageCacheRequest(*request_object);
  call.options = {options.ignore_search, options.ignore_method,
                  options.ignore_vary};
  return Ask(std::move(call), "match", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host,
                                 const PageCacheStorageAnswer& answer) {
    if (answer.error == kCacheStorageErrorNotFound) {
      std::ignore = resolver->Resolve(context, v8::Undefined(isolate));
      return;
    }
    if (answer.error != kCacheStorageSuccess || answer.responses.empty()) {
      std::ignore = resolver->Reject(
          context, CacheStorageException(isolate, answer.error, "",
                                         "match", "Cache"));
      return;
    }
    std::ignore = resolver->Resolve(
        context, bindings::ToV8(isolate,
                                ResponseFromCache(host, answer.responses[0])));
  });
}

v8::Local<v8::Promise> Cache::MatchAll(
    const std::optional<bindings::RequestOrUSVString>& request,
    const bindings::CacheQueryOptions& options,
    bindings::ExceptionState& exception_state) {
  Request* request_object = nullptr;
  if (request) {
    request_object = RequestOf(*request, exception_state);
    if (!request_object) {
      return {};
    }
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (request_object && request_object->Method() != "GET" &&
      !options.ignore_method) {
    return SettledPromise(isolate, v8::Array::New(isolate));
  }
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kCacheMatchAll;
  if (request_object) {
    call.request = ToPageCacheRequest(*request_object);
  }
  call.options = {options.ignore_search, options.ignore_method,
                  options.ignore_vary};
  return Ask(std::move(call), "matchAll", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host,
                                 const PageCacheStorageAnswer& answer) {
    if (answer.error != kCacheStorageSuccess) {
      std::ignore = resolver->Reject(
          context, CacheStorageException(isolate, answer.error, "",
                                         "matchAll", "Cache"));
      return;
    }
    std::vector<v8::Local<v8::Value>> responses;
    for (const PageCacheResponse& response : answer.responses) {
      responses.push_back(
          bindings::ToV8(isolate, ResponseFromCache(host, response)));
    }
    std::ignore = resolver->Resolve(
        context, v8::Array::New(isolate, responses.data(), responses.size()));
  });
}

v8::Local<v8::Promise> Cache::Keys(
    const std::optional<bindings::RequestOrUSVString>& request,
    const bindings::CacheQueryOptions& options,
    bindings::ExceptionState& exception_state) {
  Request* request_object = nullptr;
  if (request) {
    request_object = RequestOf(*request, exception_state);
    if (!request_object) {
      return {};
    }
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (request_object && request_object->Method() != "GET" &&
      !options.ignore_method) {
    return SettledPromise(isolate, v8::Array::New(isolate));
  }
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kCacheKeys;
  if (request_object) {
    call.request = ToPageCacheRequest(*request_object);
  }
  call.options = {options.ignore_search, options.ignore_method,
                  options.ignore_vary};
  return Ask(std::move(call), "keys", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host,
                                 const PageCacheStorageAnswer& answer) {
    if (answer.error != kCacheStorageSuccess) {
      std::ignore = resolver->Reject(
          context, CacheStorageException(isolate, answer.error, "",
                                         "keys", "Cache"));
      return;
    }
    std::vector<v8::Local<v8::Value>> requests;
    for (const PageCacheRequest& request : answer.requests) {
      requests.push_back(bindings::ToV8(isolate, RequestFromCache(request)));
    }
    std::ignore = resolver->Resolve(
        context, v8::Array::New(isolate, requests.data(), requests.size()));
  });
}

v8::Local<v8::Promise> Cache::Delete(
    const bindings::RequestOrUSVString& request,
    const bindings::CacheQueryOptions& options,
    bindings::ExceptionState& exception_state) {
  Request* const request_object = RequestOf(request, exception_state);
  if (!request_object) {
    return {};
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (request_object->Method() != "GET" && !options.ignore_method) {
    return SettledPromise(isolate, v8::False(isolate));
  }
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kCacheBatch;
  PageCacheOperation& operation = call.operations.emplace_back();
  operation.put = false;
  operation.request = ToPageCacheRequest(*request_object);
  operation.options = {options.ignore_search, options.ignore_method,
                       options.ignore_vary};
  return Ask(std::move(call), "delete", [](v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver,
                                 CacheStorageHost& host,
                                 const PageCacheStorageAnswer& answer) {
    if (answer.error == kCacheStorageSuccess) {
      std::ignore = resolver->Resolve(context, v8::True(isolate));
    } else if (answer.error == kCacheStorageErrorNotFound) {
      std::ignore = resolver->Resolve(context, v8::False(isolate));
    } else {
      std::ignore = resolver->Reject(
          context,
          CacheStorageException(
              isolate, answer.error,
              answer.message ? "Cache.delete(): " + *answer.message : "",
              "delete", "Cache"));
    }
  });
}

v8::Local<v8::Promise> Cache::Put(const bindings::RequestOrUSVString& request,
                                  Response* response,
                                  bindings::ExceptionState& exception_state) {
  Request* const request_object = RequestOf(request, exception_state);
  if (!request_object ||
      !ValidateRequestForPut(*request_object, exception_state)) {
    return {};
  }
  v8::Isolate* const isolate = exception_state.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  // The promise comes first, so that a check below can reject it.
  const uint64_t barrier =
      StartBarrier(context, resolver, "Cache.put()", {request_object});
  OnResponse(barrier, 0, response, /*require_ok=*/false);
  return resolver->GetPromise();
}

v8::Local<v8::Promise> Cache::Add(const bindings::RequestOrUSVString& request,
                                  bindings::ExceptionState& exception_state) {
  Request* const request_object = RequestOf(request, exception_state);
  if (!request_object) {
    return {};
  }
  return AddAllImpl("Cache.add()", {request_object}, exception_state);
}

v8::Local<v8::Promise> Cache::AddAll(
    const std::vector<bindings::RequestOrUSVString>& requests,
    bindings::ExceptionState& exception_state) {
  std::vector<Request*> request_objects;
  for (const bindings::RequestOrUSVString& request : requests) {
    Request* const request_object = RequestOf(request, exception_state);
    if (!request_object) {
      return {};
    }
    request_objects.push_back(request_object);
  }
  return AddAllImpl("Cache.addAll()", std::move(request_objects),
                    exception_state);
}

v8::Local<v8::Promise> Cache::AddAllImpl(
    std::string method_name,
    std::vector<Request*> requests,
    bindings::ExceptionState& exception_state) {
  v8::Isolate* const isolate = exception_state.GetIsolate();
  if (requests.empty()) {
    return SettledPromise(isolate, v8::Undefined(isolate));
  }
  // Every request is checked before any is fetched.
  for (Request* const request : requests) {
    if (!ValidateRequestForPut(*request, exception_state)) {
      return {};
    }
  }
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
    return {};
  }
  Window* const window = window_.Get();
  if (!window) {
    return {};
  }
  const uint64_t barrier =
      StartBarrier(context, resolver, std::move(method_name), requests);
  for (size_t i = 0; i < requests.size(); ++i) {
    bindings::RequestInit init;
    PutBarrier* const state = FindBarrier(barrier);
    if (state && state->abort_controller) {
      init.signal = AbortSignal::CreateComposite(
          {state->abort_controller->Signal(), requests[i]->Signal()});
    }
    const v8::Local<v8::Promise> fetch_promise =
        window->Fetch(bindings::RequestOrUSVString(requests[i]), init,
                      exception_state);
    if (exception_state.HadException() || fetch_promise.IsEmpty()) {
      barriers_.erase(barrier);
      return {};
    }
    v8::Local<v8::Value> data[] = {
        bindings::ToV8(isolate, this),
        v8::Number::New(isolate, static_cast<double>(barrier)),
        v8::Number::New(isolate, static_cast<double>(i))};
    const v8::Local<v8::Array> callback_data =
        v8::Array::New(isolate, data, std::size(data));
    v8::Local<v8::Function> on_fulfilled;
    v8::Local<v8::Function> on_rejected;
    if (!v8::Function::New(
             context,
             [](const v8::FunctionCallbackInfo<v8::Value>& info) {
               FetchSettled(info, /*fulfilled=*/true);
             },
             callback_data, 1)
             .ToLocal(&on_fulfilled) ||
        !v8::Function::New(
             context,
             [](const v8::FunctionCallbackInfo<v8::Value>& info) {
               FetchSettled(info, /*fulfilled=*/false);
             },
             callback_data, 1)
             .ToLocal(&on_rejected)) {
      return {};
    }
    std::ignore = fetch_promise->Then(context, on_fulfilled, on_rejected);
  }
  return resolver->GetPromise();
}

// static
void Cache::FetchSettled(const v8::FunctionCallbackInfo<v8::Value>& info,
                         bool fulfilled) {
  v8::Isolate* const isolate = info.GetIsolate();
  const v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const v8::Local<v8::Array> data = info.Data().As<v8::Array>();
  v8::Local<v8::Value> cache_value;
  v8::Local<v8::Value> barrier_value;
  v8::Local<v8::Value> index_value;
  if (!data->Get(context, 0).ToLocal(&cache_value) ||
      !data->Get(context, 1).ToLocal(&barrier_value) ||
      !data->Get(context, 2).ToLocal(&index_value)) {
    return;
  }
  Cache* const cache = bindings::ToWrappableOf<Cache>(
      isolate, bindings::V8Cache::kWrapperTypeInfo, cache_value);
  if (!cache) {
    return;
  }
  const uint64_t barrier =
      static_cast<uint64_t>(barrier_value.As<v8::Number>()->Value());
  const size_t index =
      static_cast<size_t>(index_value.As<v8::Number>()->Value());
  if (!fulfilled) {
    // FetchRejectHandler: the fetch's own error.
    cache->FailBarrier(barrier, info[0]);
    return;
  }
  Response* const response = bindings::ToWrappableOf<Response>(
      isolate, bindings::V8Response::kWrapperTypeInfo, info[0]);
  if (response) {
    cache->OnResponse(barrier, index, response, /*require_ok=*/true);
  }
}

uint64_t Cache::StartBarrier(v8::Local<v8::Context> context,
                             v8::Local<v8::Promise::Resolver> resolver,
                             std::string method_name,
                             std::vector<Request*> requests) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const uint64_t id = next_barrier_++;
  PutBarrier& barrier = barriers_[id];
  barrier.context.Reset(isolate, context);
  barrier.resolver.Reset(isolate, resolver);
  // "Cache.put()" is the member "put".
  barrier.member = method_name.substr(6, method_name.size() - 8);
  barrier.method_name = std::move(method_name);
  for (Request* const request : requests) {
    barrier.requests.emplace_back(request);
  }
  barrier.responses.resize(requests.size());
  barrier.bodies.resize(requests.size());
  if (requests.size() > 1) {
    barrier.abort_controller = AbortController::Create();
  }
  return id;
}

Cache::PutBarrier* Cache::FindBarrier(uint64_t barrier) {
  const auto found = barriers_.find(barrier);
  if (found == barriers_.end() || found->second.stopped) {
    return nullptr;
  }
  return &found->second;
}

void Cache::OnResponse(uint64_t barrier,
                       size_t index,
                       Response* response,
                       bool require_ok) {
  PutBarrier* const state = FindBarrier(barrier);
  if (!state) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (require_ok && !response->Ok()) {
    // OnNonOkResponse(): what failed goes to the console, and the exception
    // keeps it out of the page's sight.
    if (Window* const window = window_.Get()) {
      window->AddConsoleMessage(
          ConsoleLevel::kError,
          base::StrCat(
              {state->method_name, " failed for '",
               state->requests[index]->url().spec(),
               "': Response status ", base::NumberToString(response->Status()),
               " is not a successful status (200-299)."}));
    }
    FailBarrier(barrier, CacheCallException(isolate, "", "Request failed",
                                            state->member, "Cache"));
    return;
  }
  if (VaryHeaderContainsAsterisk(*response)) {
    FailBarrier(barrier,
                CacheCallException(isolate, "", "Vary header contains *",
                                   state->member, "Cache"));
    return;
  }
  if (response->Status() == 206) {
    FailBarrier(barrier,
                CacheCallException(isolate, "",
                                   "Partial response (status code 206) is "
                                   "unsupported",
                                   state->member, "Cache"));
    return;
  }
  if (response->BodyUsed()) {
    FailBarrier(barrier,
                CacheCallException(isolate, "", "Response body is already used",
                                   state->member, "Cache"));
    return;
  }
  state->responses[index] = response;
  if (response->HasNullBody()) {
    CompletedResponse(barrier, index, std::string());
    return;
  }
  // The body loads as a blob, from the tasks that bring it.
  response->ReadAll(base::BindOnce(
      [](cppgc::Persistent<Cache> self, uint64_t barrier, size_t index,
         std::optional<std::string> body) {
        self->CompletedResponse(barrier, index, std::move(body));
      },
      cppgc::Persistent<Cache>(this), barrier, index));
}

void Cache::CompletedResponse(uint64_t barrier,
                              size_t index,
                              std::optional<std::string> body) {
  PutBarrier* const state = FindBarrier(barrier);
  if (!state) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!body) {
    // FailedResponse().
    const v8::HandleScope handle_scope(isolate);
    const v8::Local<v8::Context> context = state->context.Get(isolate);
    const v8::Context::Scope context_scope(context);
    FailBarrier(barrier,
                CacheCallException(
                    isolate, "NetworkError",
                    state->method_name + " encountered a network error",
                    state->member, "Cache"));
    return;
  }
  state->bodies[index] = std::move(body);
  if (++state->complete == state->requests.size()) {
    PutAll(barrier);
  }
}

void Cache::FailBarrier(uint64_t barrier, v8::Local<v8::Value> exception) {
  PutBarrier* const state = FindBarrier(barrier);
  if (!state) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::Local<v8::Context> context = state->context.Get(isolate);
  if (CacheContextIsAlive(isolate, context)) {
    std::ignore = state->resolver.Get(isolate)->Reject(context, exception);
  }
  // Stop(): the other fetches abort.
  state->stopped = true;
  if (AbortController* const controller = state->abort_controller.Get()) {
    const v8::Context::Scope context_scope(context);
    controller->Abort(std::nullopt);
  }
  barriers_.erase(barrier);
}

void Cache::PutAll(uint64_t barrier) {
  PutBarrier* const state = FindBarrier(barrier);
  if (!state) {
    return;
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = state->context.Get(isolate);
  const v8::Context::Scope context_scope(context);
  CacheStorageHost* const host = Host();
  if (!host) {
    barriers_.erase(barrier);
    return;
  }
  PageCacheStorageCall call;
  call.op = CacheStorageCallOp::kCacheBatch;
  std::vector<uint64_t> bodies;
  for (size_t i = 0; i < state->requests.size(); ++i) {
    PageCacheOperation& operation = call.operations.emplace_back();
    operation.put = true;
    operation.request = ToPageCacheRequest(*state->requests[i]);
    operation.response = ToPageCacheResponse(*state->responses[i]);
    std::string& body = *state->bodies[i];
    if (!body.empty()) {
      operation.response.body_size = body.size();
      operation.response.body = host->AddPutBody(std::move(body));
      bodies.push_back(operation.response.body);
    }
  }
  const std::string method_name = state->method_name;
  const std::string member = state->member;
  const v8::Local<v8::Promise::Resolver> resolver =
      state->resolver.Get(isolate);
  barriers_.erase(barrier);
  call.cache = cache_;
  host->Ask(
      std::move(call),
      base::BindOnce(
          [](v8::Isolate* isolate, v8::Global<v8::Context> context_handle,
             v8::Global<v8::Promise::Resolver> resolver_handle,
             cppgc::Persistent<Cache> self, std::string method_name,
             std::string member, std::vector<uint64_t> bodies,
             PageCacheStorageAnswer answer) {
            for (uint64_t body : bodies) {
              self->host_->DropPutBody(body);
            }
            RunInCacheContext(
                isolate, context_handle, [&](v8::Local<v8::Context> context) {
                  const v8::Local<v8::Promise::Resolver> resolver =
                      resolver_handle.Get(isolate);
                  if (answer.error == kCacheStorageSuccess) {
                    std::ignore =
                        resolver->Resolve(context, v8::Undefined(isolate));
                    return;
                  }
                  std::ignore = resolver->Reject(
                      context,
                      CacheStorageException(
                          isolate, answer.error,
                          answer.message
                              ? base::StrCat(
                                    {method_name, ": ", *answer.message})
                              : std::string(),
                          member, "Cache"));
                });
          },
          isolate, v8::Global<v8::Context>(isolate, context),
          v8::Global<v8::Promise::Resolver>(isolate, resolver),
          cppgc::Persistent<Cache>(this), method_name, member,
          std::move(bodies)));
}

}  // namespace mitto
