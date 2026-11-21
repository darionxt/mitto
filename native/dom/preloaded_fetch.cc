#include "mitto/dom/preloaded_fetch.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/resource.h"
#include "mitto/dom/window_tasks.h"
#include "v8/include/cppgc/garbage-collected.h"

namespace mitto {

class PreloadedFetch::Observer final : public cppgc::GarbageCollected<Observer>,
                                       public ResourceClient {
 public:
  explicit Observer(base::WeakPtr<PreloadedFetch> fetch)
      : fetch_(std::move(fetch)) {}

  void Trace(cppgc::Visitor* visitor) const override {}

  // ResourceClient:
  void NotifyFinished(Resource& resource) override {
    if (PreloadedFetch* const fetch = fetch_.get()) {
      fetch->NotifyFinished();
    }
  }
  void ResponseReceived(Resource& resource) override {
    if (PreloadedFetch* const fetch = fetch_.get()) {
      fetch->Forward();
    }
  }
  void DataReceived(Resource& resource) override {
    if (PreloadedFetch* const fetch = fetch_.get()) {
      fetch->Forward();
    }
  }

 private:
  const base::WeakPtr<PreloadedFetch> fetch_;
};

// static
void PreloadedFetch::Start(Resource& resource,
                           std::unique_ptr<FetchClient> client,
                           ExecutionContext& context) {
  WindowTasks* const tasks = context.GetTasks();
  CHECK(tasks);
  auto fetch =
      base::WrapUnique(new PreloadedFetch(resource, std::move(client)));
  fetch->client_->OnStarted(*fetch);
  // A navigation lets the context go before its fetches, whose requests end
  // with no word to their clients, so the resource might never end.
  context.AddContextDestroyedCallback(base::BindOnce(
      &PreloadedFetch::Cancel, fetch->weak_factory_.GetWeakPtr()));
  // It is the resource's client at once, as ThreadableLoader is, which keeps
  // a load going that nothing else uses, and hears of what has come from a
  // task, as Resource::AddClient() tells a client.
  fetch->observer_ = bindings::MakeGarbageCollected<Observer>(
      fetch->weak_factory_.GetWeakPtr());
  resource.AddClient(fetch->observer_.Get());
  tasks->PostTask(TaskType::kNetworking,
                  base::BindOnce(&PreloadedFetch::Attach, std::move(fetch)));
}

PreloadedFetch::PreloadedFetch(Resource& resource,
                               std::unique_ptr<FetchClient> client)
    : resource_(&resource), client_(std::move(client)) {}

PreloadedFetch::~PreloadedFetch() {
  if (Observer* const observer = observer_.Get()) {
    resource_->RemoveClient(observer);
  }
}

// static
void PreloadedFetch::Attach(std::unique_ptr<PreloadedFetch> fetch) {
  if (!fetch->client_) {
    // Cancelled before.
    return;
  }
  PreloadedFetch* const self = fetch.release();
  self->delivering_ = true;
  if (self->resource_->IsLoaded()) {
    self->NotifyFinished();
  } else {
    self->Forward();
  }
}

void PreloadedFetch::NotifyFinished() {
  if (!delivering_) {
    return;
  }
  const base::WeakPtr<PreloadedFetch> weak = weak_factory_.GetWeakPtr();
  const Resource& resource = *resource_;
  if (!resource.ErrorOccurred()) {
    Forward();
    if (!weak) {
      return;
    }
  }
  // The client goes after its end, as a Fetcher's does.
  const std::unique_ptr<FetchClient> client = std::move(client_);
  client->OnResponseEnded(resource.ErrorOccurred() ? resource.NetError() : 0);
  if (weak) {
    delete this;
  }
}

void PreloadedFetch::Cancel() {
  if (!delivering_) {
    // Before the task, which destroys this: the resource may let its load go
    // now.
    if (Observer* const observer = observer_.Get()) {
      resource_->RemoveClient(observer);
      observer_.Clear();
    }
    client_.reset();
    return;
  }
  delete this;
}

void PreloadedFetch::Forward() {
  const base::WeakPtr<PreloadedFetch> weak = weak_factory_.GetWeakPtr();
  const Resource& resource = *resource_;
  if (!delivering_ || !client_) {
    return;
  }
  if (!response_started_) {
    // A response starts with its status.
    if (!resource.HttpStatus() || resource.ErrorOccurred()) {
      return;
    }
    response_started_ = true;
    client_->OnResponseStarted(resource.Response());
    if (!weak || !client_) {
      return;
    }
  }
  const std::string& body = resource.Body();
  if (body.size() > body_sent_) {
    std::string bytes = body.substr(body_sent_);
    body_sent_ = body.size();
    client_->OnResponseBody(std::move(bytes));
  }
}

}  // namespace mitto
