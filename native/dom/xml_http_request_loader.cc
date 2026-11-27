#include "mitto/dom/xml_http_request_loader.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/preloaded_fetch.h"
#include "mitto/dom/window_task.h"
#include "mitto/dom/xml_http_request.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {
namespace {

// net::ERR_TIMED_OUT, which the Page program, with no //net, cannot name.
constexpr int kTimedOut = -7;

}  // namespace

// The Fetcher's side of the request, which keeps the loader, and through it
// the XMLHttpRequest, while it is open.
class XMLHttpRequestLoader::RequestClient final : public FetchClient {
 public:
  explicit RequestClient(XMLHttpRequestLoader* loader) : loader_(loader) {}
  ~RequestClient() override { loader_->controller_ = nullptr; }

  void OnStarted(FetchController& controller) override {
    loader_->controller_ = &controller;
  }
  void OnResponseStarted(const FetchResponseHead& head) override {
    loader_->CallClient(
        [&](XMLHttpRequest& xhr) { xhr.DidReceiveResponse(head); });
  }
  void OnResponseBody(std::string bytes) override {
    // The bytes are taken as they come, as Blink reads the body's data pipe. A
    // synchronous request takes them itself.
    if (loader_->controller_) {
      loader_->controller_->TookBody(bytes.size());
    }
    loader_->CallClient(
        [&](XMLHttpRequest& xhr) { xhr.DidReceiveData(bytes); });
  }
  void OnUploadProgress(uint64_t position, uint64_t size) override {
    loader_->CallClient(
        [&](XMLHttpRequest& xhr) { xhr.DidSendData(position, size); });
  }
  void OnResponseEnded(int net_error) override {
    loader_->controller_ = nullptr;
    // A request that cannot be sent, as one too large for a message, fails from
    // a task.
    if (loader_->starting_) {
      loader_->EndLater(net_error);
      return;
    }
    loader_->DidEnd(net_error);
  }

 private:
  const cppgc::Persistent<XMLHttpRequestLoader> loader_;
};

XMLHttpRequestLoader::XMLHttpRequestLoader(XMLHttpRequest* client,
                                           ExecutionContext* context)
    : client_(client), context_(context) {}

XMLHttpRequestLoader::~XMLHttpRequestLoader() = default;

void XMLHttpRequestLoader::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(client_);
  visitor->Trace(context_);
}

void XMLHttpRequestLoader::SetTimeout(base::TimeDelta timeout) {
  timeout_ = timeout;
  // Before its start and after its end there is nothing to time.
  if (request_started_.is_null()) {
    return;
  }
  // The timer that runs stops, and the time counts from the start.
  ++timer_;
  if (!timeout_.is_zero()) {
    StartTimer(std::max(timeout_ - (base::TimeTicks::Now() - request_started_),
                        base::TimeDelta()));
  }
}

void XMLHttpRequestLoader::Start(Fetcher& fetcher, FetchRequestData data) {
  DidStart();
  starting_ = true;
  fetcher.Fetch(std::move(data), std::make_unique<RequestClient>(this));
  starting_ = false;
}

void XMLHttpRequestLoader::Start(LocalSchemeFetch::Answer answer) {
  DidStart();
  LocalSchemeFetch::Start(std::move(answer),
                          std::make_unique<RequestClient>(this), *context_);
}

void XMLHttpRequestLoader::Start(Resource& preload) {
  DidStart();
  PreloadedFetch::Start(preload, std::make_unique<RequestClient>(this),
                        *context_);
}

void XMLHttpRequestLoader::StartFailing(int net_error) {
  DidStart();
  EndLater(net_error);
}

void XMLHttpRequestLoader::EndLater(int net_error) {
  PostTaskInContext(
      context_.Get(), TaskType::kNetworking,
      base::BindOnce([](cppgc::Persistent<XMLHttpRequestLoader> loader,
                        int net_error) { loader->DidEnd(net_error); },
                     cppgc::Persistent<XMLHttpRequestLoader>(this), net_error));
}

void XMLHttpRequestLoader::RunSynchronously(Fetcher& fetcher,
                                            FetchRequestData data) {
  synchronous_ = true;
  RequestClient client(this);
  fetcher.FetchSynchronously(std::move(data), timeout_, client);
}

void XMLHttpRequestLoader::RunSynchronously(LocalSchemeFetch::Answer answer) {
  synchronous_ = true;
  if (answer.net_error != 0) {
    DidEnd(answer.net_error);
    return;
  }
  CallClient([&](XMLHttpRequest& xhr) { xhr.DidReceiveResponse(answer.head); });
  CallClient(
      [&](XMLHttpRequest& xhr) { xhr.DidReceiveData(answer.TakeBody()); });
  DidEnd(0);
}

void XMLHttpRequestLoader::Cancel() {
  Clear();
  if (controller_) {
    // Under a call to the client, the client goes once that has returned.
    std::exchange(controller_, nullptr)->Cancel();
  }
}

void XMLHttpRequestLoader::DidStart() {
  request_started_ = base::TimeTicks::Now();
  if (!timeout_.is_zero()) {
    StartTimer(timeout_);
  }
}

void XMLHttpRequestLoader::Run(base::FunctionRef<void()> run) {
  if (synchronous_) {
    run();
    return;
  }
  RunInContext(context_.Get(), run);
}

void XMLHttpRequestLoader::CallClient(
    base::FunctionRef<void(XMLHttpRequest&)> call) {
  Run([&] {
    if (client_) {
      call(*client_);
    }
  });
}

void XMLHttpRequestLoader::DidEnd(int net_error) {
  Run([&] {
    XMLHttpRequest* const client = client_.Get();
    if (!client) {
      return;
    }
    Clear();
    if (net_error == 0) {
      client->DidFinishLoading();
    } else {
      client->DidFail(net_error);
    }
  });
}

void XMLHttpRequestLoader::DidTimeout() {
  XMLHttpRequest* const client = client_.Get();
  Cancel();
  if (client) {
    client->DidFail(kTimedOut);
  }
}

void XMLHttpRequestLoader::StartTimer(base::TimeDelta delay) {
  // From a task of the context, which waits while the back/forward cache keeps
  // its window, as Blink's timer on the context's task runner does.
  PostDelayedTaskInContext(
      context_.Get(), TaskType::kNetworking,
      base::BindOnce(
          [](cppgc::WeakPersistent<XMLHttpRequestLoader> loader,
             uint64_t timer) {
            if (loader && loader->timer_ == timer) {
              loader->DidTimeout();
            }
          },
          cppgc::WeakPersistent<XMLHttpRequestLoader>(this), ++timer_),
      delay);
}

void XMLHttpRequestLoader::Clear() {
  client_ = nullptr;
  request_started_ = base::TimeTicks();
  ++timer_;
}

}  // namespace mitto
