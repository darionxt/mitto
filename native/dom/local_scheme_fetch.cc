#include "mitto/dom/local_scheme_fetch.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
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

// A response's URL, which never has a fragment.
std::string ResponseUrlOf(const GURL& url) {
  return url.GetWithoutRef().spec();
}

}  // namespace

LocalSchemeFetch::Answer::Answer() = default;
LocalSchemeFetch::Answer::Answer(Answer&&) = default;
LocalSchemeFetch::Answer& LocalSchemeFetch::Answer::operator=(Answer&&) =
    default;
LocalSchemeFetch::Answer::~Answer() = default;

// static
LocalSchemeFetch::Answer LocalSchemeFetch::AnswerDataUrl(
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
  base::StrAppend(&answer.head.headers,
                  {"Content-Type: ", blob->GetType(), "\n"});
  answer.body = std::string(base::as_string_view(blob->Bytes().subspan(
      static_cast<size_t>(first),
      static_cast<size_t>(std::max<int64_t>(length, 0)))));
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
  // with no word.
  context.AddContextDestroyedCallback(base::BindOnce(
      &LocalSchemeFetch::Cancel, fetch->weak_factory_.GetWeakPtr()));
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

LocalSchemeFetch::~LocalSchemeFetch() = default;

void LocalSchemeFetch::Cancel() {
  delete this;
}

void LocalSchemeFetch::Post(void (LocalSchemeFetch::*step)()) {
  PostTaskInContext(context_.Get(), TaskType::kNetworking,
                    base::BindOnce(step, weak_factory_.GetWeakPtr()));
}

void LocalSchemeFetch::PostRespond() {
  Post(&LocalSchemeFetch::Respond);
}

void LocalSchemeFetch::Respond() {
  if (answer_.net_error) {
    const std::unique_ptr<FetchClient> client = std::move(client_);
    const int net_error = answer_.net_error;
    delete this;
    client->OnResponseEnded(net_error);
    return;
  }
  const base::WeakPtr<LocalSchemeFetch> weak = weak_factory_.GetWeakPtr();
  client_->OnResponseStarted(answer_.head);
  if (!weak) {
    return;
  }
  SendResponseBody();
}

void LocalSchemeFetch::SendResponseBody() {
  const base::WeakPtr<LocalSchemeFetch> weak = weak_factory_.GetWeakPtr();
  const size_t rest = answer_.body.size() - body_sent_;
  if (rest) {
    // A data: URL's body comes whole; a blob's a pipe's worth at a time.
    const size_t piece =
        answer_.from_browser ? std::min(rest, answer_.pipe_capacity) : rest;
    std::string bytes = answer_.body.substr(body_sent_, piece);
    body_sent_ += piece;
    client_->OnResponseBody(std::move(bytes));
    if (!weak) {
      return;
    }
    if (body_sent_ < answer_.body.size()) {
      Post(&LocalSchemeFetch::SendResponseBody);
      return;
    }
  }
  // The client goes after its end, as a Fetcher's does.
  const std::unique_ptr<FetchClient> client = std::move(client_);
  delete this;
  client->OnResponseEnded(0);
}

}  // namespace mitto
