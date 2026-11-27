#include "mitto/dom/cache_storage_answers.h"

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_messages.h"
#include "mitto/bindings/per_isolate_data.h"
#include "mitto/dom/body_stream_buffer.h"
#include "mitto/dom/cache_body_loader.h"
#include "mitto/dom/cache_storage_host.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/quota_exceeded_error.h"
#include "mitto/dom/request.h"
#include "mitto/dom/response.h"
#include "mitto/header_rules.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// GetDefaultMessage().
std::string DefaultMessage(uint8_t error) {
  switch (error) {
    case 1:
      return "Entry already exists.";
    case kCacheStorageErrorStorage:
      return "Unexpected internal error.";
    case kCacheStorageErrorNotFound:
      return "Entry was not found.";
    case kCacheStorageErrorQuotaExceeded:
      return "Quota exceeded.";
    case kCacheStorageErrorCacheNameNotFound:
      return "Cache was not found.";
    case kCacheStorageErrorQueryTooLarge:
      return "Operation too large.";
    case 7:
      return "Method is not implemented.";
    case 8:
      return "Duplicate operation.";
    case 9:
      return "Failed Cross-Origin-Resource-Policy check.";
    default:
      return std::string();
  }
}

// RejectCacheStorageWithError()'s exception names.
std::string ErrorName(uint8_t error) {
  switch (error) {
    case 1:
      return "InvalidAccessError";
    case kCacheStorageErrorNotFound:
    case kCacheStorageErrorCacheNameNotFound:
      return "NotFoundError";
    case kCacheStorageErrorQueryTooLarge:
      return "AbortError";
    case 7:
      return "NotSupportedError";
    case 8:
      return "InvalidStateError";
    case kCacheStorageErrorStorage:
    default:
      return "UnknownError";
  }
}

}  // namespace

v8::Local<v8::Value> CacheStorageException(v8::Isolate* isolate,
                                           uint8_t error,
                                           const std::string& message,
                                           std::string_view method,
                                           std::string_view interface_name) {
  if (error == 9) {
    // kErrorCrossOriginResourcePolicy rejects with a TypeError of the
    // message it was given, which may be empty.
    return CacheCallException(isolate, "", message, method, interface_name);
  }
  const std::string final_message =
      message.empty() ? DefaultMessage(error) : message;
  if (error == kCacheStorageErrorQuotaExceeded &&
      bindings::PerIsolateData::From(isolate)->features().IsEnabled(
          bindings::RuntimeFeature::kQuotaExceededErrorUpdate)) {
    // QuotaExceededError::Reject(): the interface of its own with
    // QuotaExceededErrorUpdate, which is on, and with neither quota nor
    // requested, made without the resolver's context, so its message names no
    // call.
    return QuotaExceededError::Create(base::UTF8ToUTF16(final_message),
                                      std::nullopt, std::nullopt)
        ->ToV8(isolate);
  }
  return CacheCallException(
      isolate,
      error == kCacheStorageErrorQuotaExceeded ? "QuotaExceededError"
                                               : ErrorName(error),
      final_message, method, interface_name);
}

v8::Local<v8::Value> CacheCallException(v8::Isolate* isolate,
                                        std::string_view name,
                                        std::string_view message,
                                        std::string_view method,
                                        std::string_view interface_name) {
  const std::string in_context = bindings::ExceptionMessages::FailedToExecute(
      method, interface_name, message);
  if (name.empty()) {
    return v8::Exception::TypeError(
        v8::String::NewFromUtf8(isolate, in_context.data(),
                                v8::NewStringType::kNormal,
                                static_cast<int>(in_context.size()))
            .ToLocalChecked());
  }
  return bindings::PerIsolateData::From(isolate)->dom_exception_hooks().create(
      isolate, std::string(name), in_context);
}

PageCacheRequest ToPageCacheRequest(const Request& request) {
  PageCacheRequest page_request;
  page_request.url = request.url().spec();
  page_request.method = request.Method();
  // Each name once, its values joined with ", ", in the order of the first
  // of each, without Referer.
  std::vector<std::pair<std::string, std::string>> joined;
  std::map<std::string, size_t> index;
  for (const auto& [name, value] : request.GetHeaders()->list()) {
    const std::string lower = base::ToLowerASCII(name);
    if (lower == "referer") {
      continue;
    }
    const auto [found, inserted] = index.emplace(lower, joined.size());
    if (inserted) {
      joined.emplace_back(name, value);
    } else {
      joined[found->second].second += ", " + value;
    }
  }
  page_request.headers = std::move(joined);
  return page_request;
}

Request* RequestFromCache(const PageCacheRequest& request) {
  return Request::CreateFromStored(GURL(request.url), request.method,
                                   request.headers);
}

Response* ResponseFromCache(CacheStorageHost& host,
                            const PageCacheResponse& response) {
  // network.mojom.FetchResponseType: basic, cors, default, error and opaque
  // are what the Runtime's responses can be, and what a page put.
  Response::ResponseType type = Response::ResponseType::kDefault;
  if (response.type == 0) {
    type = Response::ResponseType::kBasic;
  } else if (response.type == 1) {
    type = Response::ResponseType::kCors;
  } else if (response.type == 3) {
    // A network error's, as Response.error() gives, which needs nothing
    // else.
    return Response::Error();
  } else if (response.type == 4) {
    type = Response::ResponseType::kOpaque;
  }
  // The filtered response of its type: no Set-Cookie for a basic one, and of
  // a CORS one only the safelisted names and the exposed ones.
  const std::set<std::string> exposed(
      response.cors_exposed_header_names.begin(),
      response.cors_exposed_header_names.end());
  Headers* const headers = Headers::Create(Headers::Guard::kNone);
  for (const auto& [name, value] : response.headers) {
    if (type != Response::ResponseType::kDefault &&
        IsForbiddenResponseHeaderName(name)) {
      continue;
    }
    if (type == Response::ResponseType::kCors &&
        !IsCorsSafelistedResponseHeaderName(name) &&
        !exposed.contains(base::ToLowerASCII(name)) && !exposed.contains("*")) {
      continue;
    }
    headers->AppendLine(name, value);
  }
  headers->set_guard(Headers::Guard::kImmutable);
  const std::u16string url = response.url_list.empty()
                                 ? std::u16string()
                                 : base::UTF8ToUTF16(response.url_list.back());
  if (response.body == 0) {
    Response* const fetched = Response::CreateFetched(
        type, response.status, response.status_text, url,
        response.url_list.size() > 1, headers, nullptr);
    fetched->SetPadding(response.padding);
    return fetched;
  }
  // The loader goes to the host, and its consumer to the body, which drops
  // the stored body once it goes.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  auto loader = std::make_unique<CacheBodyLoader>(host, response.body, isolate,
                                                  isolate->GetCurrentContext());
  BodyStreamBuffer* const body = BodyStreamBuffer::Create(
      isolate, loader->consumer(), /*signal=*/nullptr);
  host.AdoptStoredBody(response.body, std::move(loader));
  Response* const fetched =
      Response::CreateFetched(type, response.status, response.status_text, url,
                              response.url_list.size() > 1, headers, body);
  fetched->SetPadding(response.padding);
  return fetched;
}

bool CacheContextIsAlive(v8::Isolate* isolate,
                         v8::Local<v8::Context> context) {
  const ExecutionContext* const execution_context =
      ExecutionContext::From(isolate, context);
  return execution_context && execution_context->HasContext();
}

void RunInCacheContext(v8::Isolate* isolate,
                       const v8::Global<v8::Context>& context_handle,
                       base::FunctionRef<void(v8::Local<v8::Context>)> run) {
  const v8::HandleScope handle_scope(isolate);
  const v8::Local<v8::Context> context = context_handle.Get(isolate);
  if (!CacheContextIsAlive(isolate, context)) {
    return;
  }
  const v8::Context::Scope context_scope(context);
  const v8::MicrotasksScope microtasks_scope(
      context, v8::MicrotasksScope::kRunMicrotasks);
  run(context);
}

}  // namespace mitto
