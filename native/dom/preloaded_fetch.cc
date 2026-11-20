#include "mitto/dom/preloaded_fetch.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ptr_util.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/event_loop.h"
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
                           WindowTasks& tasks) {
  auto fetch =
      base::WrapUnique(new PreloadedFetch(resource, std::move(client)));
  fetch->client_->OnStarted(*fetch);
  // Resource::AddClient() tells a client of what has come from a task.
  tasks.PostTask(TaskType::kNetworking,
                 base::BindOnce(&PreloadedFetch::Attach, std::move(fetch)));
}

PreloadedFetch::PreloadedFetch(Resource& resource,
                               std::unique_ptr<FetchClient> client)
    : resource_(&resource), client_(std::move(client)) {}

PreloadedFetch::~PreloadedFetch() = default;

// static
void PreloadedFetch::Attach(std::unique_ptr<PreloadedFetch> fetch) {
  if (!fetch->client_) {
    // Cancelled before.
    return;
  }
  PreloadedFetch* const self = fetch.release();
  const base::WeakPtr<PreloadedFetch> weak = self->weak_factory_.GetWeakPtr();
  self->observer_ = bindings::MakeGarbageCollected<Observer>(weak);
  // A resource that has ended tells its client at once.
  self->resource_->AddClient(self->observer_.Get());
  if (weak) {
    self->Forward();
  }
}

void PreloadedFetch::NotifyFinished() {
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
    End();
  }
}

void PreloadedFetch::Cancel() {
  if (!observer_) {
    // Before the task, which destroys this.
    client_.reset();
    return;
  }
  End();
}

void PreloadedFetch::Forward() {
  const base::WeakPtr<PreloadedFetch> weak = weak_factory_.GetWeakPtr();
  const Resource& resource = *resource_;
  if (!client_) {
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

void PreloadedFetch::End() {
  resource_->RemoveClient(observer_.Get());
  delete this;
}

}  // namespace mitto
