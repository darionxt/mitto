#include "mitto/dom/fetch_loader.h"

#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_conversions.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/response.h"
#include "mitto/header_rules.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// The Fetch Standard's null body statuses.
bool IsNullBodyStatus(int32_t status) {
  return status == 101 || status == 103 || status == 204 || status == 205 ||
         status == 304;
}

// The names that Access-Control-Expose-Headers lists, in lower case. A
// request without credentials, as each of the Runtime's cross-origin ones
// is, exposes every name with "*".
std::set<std::string> ExposedHeaderNames(const Headers& headers) {
  std::set<std::string> names;
  const std::optional<std::string> list =
      headers.GetValue("access-control-expose-headers");
  if (!list) {
    return names;
  }
  for (const std::string_view name : base::SplitStringPiece(
           *list, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (IsHttpToken(name) || name == "*") {
      names.insert(base::ToLowerASCII(name));
    }
  }
  return names;
}

}  // namespace

FetchLoader::FetchLoader(v8::Isolate* isolate,
                         v8::Local<v8::Context> context,
                         v8::Local<v8::Promise::Resolver> resolver,
                         bool head)
    : isolate_(isolate),
      context_(isolate, context),
      resolver_(isolate, resolver),
      head_(head) {}

FetchLoader::~FetchLoader() {
  if (Response* const response = response_.Get()) {
    response->loader_ = nullptr;
  }
}

void FetchLoader::OnStarted(FetchController& controller) {
  controller_ = &controller;
}

void FetchLoader::OnResponseStarted(const FetchResponseHead& head) {
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  const v8::Context::Scope context_scope(context);
  const bool cors = head.cors;
  // What a script reads of the response's header lines: no Set-Cookie, and of
  // a cross-origin response only the safelisted names and the exposed ones,
  // as Blink's FetchResponseData filters them.
  Headers* const all = Headers::Create(Headers::Guard::kNone);
  for (const std::string_view line :
       base::SplitStringPiece(head.headers, "\n", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    const size_t colon = line.find(": ");
    if (colon != std::string_view::npos) {
      all->AppendLine(std::string(line.substr(0, colon)),
                      std::string(line.substr(colon + 2)));
    }
  }
  Headers* const headers = Headers::Create(Headers::Guard::kNone);
  const std::set<std::string> exposed =
      cors ? ExposedHeaderNames(*all) : std::set<std::string>();
  for (const auto& [name, value] : all->list()) {
    if (IsForbiddenResponseHeaderName(name)) {
      continue;
    }
    if (cors && !IsCorsSafelistedResponseHeaderName(name) &&
        !exposed.contains(base::ToLowerASCII(name)) && !exposed.contains("*")) {
      continue;
    }
    headers->AppendLine(name, value);
  }
  headers->set_guard(Headers::Guard::kImmutable);
  // The Fetch Standard's null body: a HEAD request's, or a null body status's.
  const bool null_body = head_ || IsNullBodyStatus(head.status);
  Response* const response = Response::CreateFetched(
      cors ? Response::ResponseType::kCors : Response::ResponseType::kBasic,
      head.status, head.status_text, base::UTF8ToUTF16(head.url),
      head.redirected, headers, null_body ? nullptr : this);
  response_ = response;
  std::ignore = resolver_.Get(isolate_)->Resolve(
      context, bindings::ToV8(isolate_, response));
  resolver_.Reset();
}

void FetchLoader::OnResponseBody(std::string bytes) {
  Response* const response = response_.Get();
  if (!response) {
    // The Response went, and the request is about to be cancelled.
    return;
  }
  if (!response->loader_) {
    // A null body: whatever the server sent goes nowhere.
    controller_->TookBody(bytes.size());
    return;
  }
  untaken_ += bytes.size();
  response->AppendBody(bytes);
  if (response->HasReader()) {
    const size_t taken = untaken_;
    untaken_ = 0;
    controller_->TookBody(taken);
  }
}

void FetchLoader::OnResponseEnded(int net_error) {
  const v8::HandleScope handle_scope(isolate_);
  const v8::Local<v8::Context> context = context_.Get(isolate_);
  const v8::Context::Scope context_scope(context);
  if (!resolver_.IsEmpty()) {
    // Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
    std::ignore = resolver_.Get(isolate_)->Reject(
        context, v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
                     isolate_, "Failed to fetch")));
    resolver_.Reset();
    return;
  }
  if (Response* const response = response_.Get()) {
    response->loader_ = nullptr;
    response->EndBody(net_error);
  }
  reading_.Clear();
}

void FetchLoader::ReaderStarted() {
  reading_ = response_.Get();
  if (untaken_ > 0) {
    const size_t taken = untaken_;
    untaken_ = 0;
    controller_->TookBody(taken);
  }
}

void FetchLoader::ResponseCollected() {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&FetchLoader::Cancel, weak_factory_.GetWeakPtr()));
}

void FetchLoader::Cancel() {
  // Destroys this.
  controller_->Cancel();
}

}  // namespace mitto
