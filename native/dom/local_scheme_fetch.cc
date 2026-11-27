#include "mitto/dom/local_scheme_fetch.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/string_view_util.h"
#include "mitto/dom/data_url.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/http_byte_range.h"
#include "mitto/dom/window_task.h"
#include "url/gurl.h"

namespace mitto {
namespace {

// The net errors that the Page program, with no //net, names by number.
constexpr int kFailed = -2;
constexpr int kFileNotFound = -6;
constexpr int kInvalidUrl = -300;
constexpr int kMethodNotSupported = -322;
constexpr int kRequestRangeNotSatisfiable = -328;

// Blink's kBlobMinDataPipeCapacity and kBlobDefaultDataPipeCapacity.
constexpr size_t kMinDataPipeCapacity = 1024;
constexpr size_t kDefaultDataPipeCapacity = 2 * 1024 * 1024;

// A response's URL: the request's, with its fragment, as the Fetch
// Standard's URL list keeps it, which a module's import.meta.url gives.
// Response's url getter leaves the fragment out.
std::string ResponseUrlOf(const GURL& url) {
  return url.spec();
}

}  // namespace

LocalSchemeFetch::Answer::Answer() = default;
LocalSchemeFetch::Answer::Answer(Answer&&) = default;
LocalSchemeFetch::Answer& LocalSchemeFetch::Answer::operator=(Answer&&) =
    default;
LocalSchemeFetch::Answer::~Answer() = default;

std::string LocalSchemeFetch::Answer::TakeBody() {
  if (!blob) {
    return std::move(body);
  }
  std::string bytes(
      base::as_string_view(blob->Bytes().subspan(blob_offset, blob_length)));
  blob = nullptr;
  return bytes;
}

// static
LocalSchemeFetch::Answer LocalSchemeFetch::AnswerDataUrl(
    const GURL& url,
    std::string_view method) {
  // ResourceLoader::RequestAsynchronously() posts HandleDataUrl() before it
  // parses anything, so the URL is decoded in the task, which an abort before
  // it spares.
  Answer answer;
  answer.data_url = url;
  answer.method = std::string(method);
  return answer;
}

// static
LocalSchemeFetch::Answer LocalSchemeFetch::DecodeDataUrl(
    const GURL& url,
    std::string_view method) {
  Answer answer;
  std::optional<DataUrlResponse> data = ParseDataUrl(url);
  if (!data) {
    answer.net_error = kInvalidUrl;
    return answer;
  }
  answer.head.status = 200;
  answer.head.status_text = "OK";
  answer.head.url = ResponseUrlOf(url);
  answer.head.headers =
      base::StrCat({"Content-Type: ", data->content_type, "\n"});
  // net::DataURL::BuildResponse() gives no body for HEAD.
  if (method != "HEAD") {
    answer.body = std::move(data->body);
  }
  return answer;
}

// static
LocalSchemeFetch::Answer LocalSchemeFetch::AnswerBlobUrl(
    const GURL& url,
    scoped_refptr<BlobDataHandle> blob,
    std::string_view method,
    const std::optional<std::string>& range) {
  Answer answer;
  answer.from_browser = true;
  if (!blob) {
    answer.net_error = kFileNotFound;
    return answer;
  }
  // Only GET, as the File API says.
  if (method != "GET") {
    answer.net_error = kMethodNotSupported;
    return answer;
  }
  const int64_t total_size = base::checked_cast<int64_t>(blob->size());
  HttpByteRange byte_range;
  bool byte_range_set = false;
  if (range) {
    // kBlobURLFetchRangeHeaderValidation, which Chrome 157 has on: the Fetch
    // Standard's single-range parser, and failure for anything else.
    std::optional<HttpByteRange> parsed =
        ParseFetchSingleRange(*range, /*allow_whitespace=*/true);
    if (!parsed || !parsed->IsValid()) {
      answer.net_error = kRequestRangeNotSatisfiable;
      return answer;
    }
    byte_range = *parsed;
    byte_range_set = true;
  }
  // MojoBlobReader: a range that fits nothing of the blob fails.
  if (!byte_range.ComputeBounds(total_size)) {
    answer.net_error = kRequestRangeNotSatisfiable;
    return answer;
  }
  const int64_t first = byte_range.first_byte_position();
  const int64_t length = byte_range.last_byte_position() - first + 1;
  answer.pipe_capacity = std::max(kMinDataPipeCapacity,
                                  static_cast<size_t>(std::min<uint64_t>(
                                      blob->size(), kDefaultDataPipeCapacity)));
  // GenerateHeaders(): Content-Length, then Content-Range for a range, then
  // Content-Type, empty for a blob of no type. Blink gives such a response
  // the status text OK, whatever its status.
  answer.head.status = byte_range_set ? 206 : 200;
  answer.head.status_text = "OK";
  answer.head.url = ResponseUrlOf(url);
  answer.head.headers =
      base::StrCat({"Content-Length: ", base::NumberToString(length), "\n"});
  if (byte_range_set) {
    base::StrAppend(&answer.head.headers,
                    {"Content-Range: bytes ", base::NumberToString(first), "-",
                     base::NumberToString(byte_range.last_byte_position()), "/",
                     base::NumberToString(total_size), "\n"});
  }
  // A header's value, which a blob's type may start or end with spaces in,
  // as HttpResponseHeaders trims it.
  base::StrAppend(
      &answer.head.headers,
      {"Content-Type: ",
       base::TrimWhitespaceASCII(blob->GetType(), base::TRIM_ALL), "\n"});
  // The blob's bytes stay where they are, and each piece is copied as it
  // goes, as MojoBlobReader reads them into the pipe.
  answer.blob = std::move(blob);
  answer.blob_offset = static_cast<size_t>(first);
  answer.blob_length = static_cast<size_t>(std::max<int64_t>(length, 0));
  return answer;
}

// static
LocalSchemeFetch::Answer LocalSchemeFetch::AnswerRefusedBlobUrl() {
  Answer answer;
  answer.from_browser = true;
  answer.net_error = kFailed;
  return answer;
}

// static
void LocalSchemeFetch::Start(Answer answer,
                             std::unique_ptr<FetchClient> client,
                             ExecutionContext& context) {
  auto* const fetch =
      new LocalSchemeFetch(std::move(answer), std::move(client), context);
  fetch->client_->OnStarted(*fetch);
  // A navigation lets the context go before its fetches, whose clients go
  // with no word. A fetch that ends first takes its callback back.
  fetch->context_destroyed_callback_ = context.AddContextDestroyedCallback(
      base::BindOnce(&LocalSchemeFetch::OnContextDestroyed,
                     fetch->weak_factory_.GetWeakPtr()));
  if (fetch->answer_.from_browser) {
    // The browser hears of the request from one task, and answers in another.
    fetch->Post(&LocalSchemeFetch::PostRespond);
  } else {
    fetch->Post(&LocalSchemeFetch::Respond);
  }
}

LocalSchemeFetch::LocalSchemeFetch(Answer answer,
                                   std::unique_ptr<FetchClient> client,
                                   ExecutionContext& context)
    : answer_(std::move(answer)),
      client_(std::move(client)),
      context_(&context) {}

LocalSchemeFetch::~LocalSchemeFetch() {
  if (ExecutionContext* const context = context_.Get()) {
    context->RemoveContextDestroyedCallback(context_destroyed_callback_);
  }
}

void LocalSchemeFetch::Cancel() {
  // A cancel under a call to the client, as when a script that resolving the
  // fetch's promise runs aborts it or lets the context go: the client goes
  // once the call has returned, not under it.
  if (calling_client_) {
    cancel_pending_ = true;
    return;
  }
  delete this;
}

void LocalSchemeFetch::OnContextDestroyed() {
  Cancel();
}

bool LocalSchemeFetch::ReturnedFromClient() {
  calling_client_ = false;
  if (cancel_pending_) {
    delete this;
    return false;
  }
  return true;
}

void LocalSchemeFetch::Post(void (LocalSchemeFetch::*step)()) {
  PostTaskInContext(context_.Get(), TaskType::kNetworking,
                    base::BindOnce(step, weak_factory_.GetWeakPtr()));
}

void LocalSchemeFetch::PostRespond() {
  Post(&LocalSchemeFetch::Respond);
}

void LocalSchemeFetch::Respond() {
  if (answer_.data_url) {
    answer_ = DecodeDataUrl(*answer_.data_url, answer_.method);
  }
  if (answer_.net_error) {
    End(answer_.net_error);
    return;
  }
  const base::WeakPtr<LocalSchemeFetch> weak = weak_factory_.GetWeakPtr();
  calling_client_ = true;
  client_->OnResponseStarted(answer_.head);
  if (!weak || !ReturnedFromClient()) {
    return;
  }
  SendResponseBody();
}

void LocalSchemeFetch::TookBody(size_t bytes) {
  in_pipe_ -= std::min(in_pipe_, bytes);
  // The pipe had no room for the next piece, which can come now.
  if (waiting_for_room_ && in_pipe_ < answer_.pipe_capacity) {
    waiting_for_room_ = false;
    Post(&LocalSchemeFetch::SendResponseBody);
  }
}

void LocalSchemeFetch::SendResponseBody() {
  const base::WeakPtr<LocalSchemeFetch> weak = weak_factory_.GetWeakPtr();
  const size_t size = BodySize();
  const size_t rest = size - body_sent_;
  if (rest) {
    // A data: URL's body comes whole; a blob's a pipe's worth at a time, as
    // much as the pipe has room for, which the client makes as it takes it.
    if (answer_.blob && in_pipe_ >= answer_.pipe_capacity) {
      waiting_for_room_ = true;
      return;
    }
    const size_t piece =
        answer_.blob ? std::min(rest, answer_.pipe_capacity - in_pipe_) : rest;
    std::string bytes;
    if (answer_.blob) {
      bytes = std::string(base::as_string_view(answer_.blob->Bytes().subspan(
          answer_.blob_offset + body_sent_, piece)));
      in_pipe_ += piece;
    } else {
      bytes = answer_.body.substr(body_sent_, piece);
    }
    body_sent_ += piece;
    calling_client_ = true;
    client_->OnResponseBody(std::move(bytes));
    if (!weak || !ReturnedFromClient()) {
      return;
    }
    if (body_sent_ < size) {
      Post(&LocalSchemeFetch::SendResponseBody);
      return;
    }
  }
  End(0);
}

void LocalSchemeFetch::End(int net_error) {
  // The client goes after its end, as a Fetcher's does, and this after it,
  // since page code that the end runs may abort the fetch.
  calling_client_ = true;
  client_->OnResponseEnded(net_error);
  delete this;
}

}  // namespace mitto
