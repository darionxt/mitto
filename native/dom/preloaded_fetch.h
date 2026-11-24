// A fetch() that takes what a link preloaded as fetch, as Blink's
// ResourceFetcher::MatchPreload() gives ThreadableLoader the document's raw
// resource: it makes no request of its own, and its client hears the
// resource's response and body as a RawResource's client does, from a task
// for what has come already and as the rest comes.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/raw_resource.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource.cc

#ifndef MITTO_DOM_PRELOADED_FETCH_H_
#define MITTO_DOM_PRELOADED_FETCH_H_

#include <cstddef>
#include <memory>

#include "base/memory/weak_ptr.h"
#include "mitto/dom/fetcher.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class ExecutionContext;
class Resource;

class PreloadedFetch final : public FetchController {
 public:
  // Starts `client` on `resource`, which it hears of from a task of
  // `context`, a window with tasks. It owns itself until the client's end,
  // or until the context goes, which destroys the client with no word, as
  // Blink's FetchManager disposes its loaders when its context is destroyed.
  static void Start(Resource& resource,
                    std::unique_ptr<FetchClient> client,
                    ExecutionContext& context);

  PreloadedFetch(const PreloadedFetch&) = delete;
  PreloadedFetch& operator=(const PreloadedFetch&) = delete;
  ~PreloadedFetch() override;

  // FetchController: the resource keeps its whole body, so taking it lets
  // nothing more come.
  void TookBody(size_t bytes) override {}
  void Cancel() override;

 private:
  // The resource's client, which the resource holds weakly.
  class Observer;

  PreloadedFetch(Resource& resource, std::unique_ptr<FetchClient> client);

  static void Attach(std::unique_ptr<PreloadedFetch> fetch);
  // The client hears of the end, and then this is destroyed.
  void NotifyFinished();
  // Tells the client of the response, once it has started, and of the body
  // that it has not heard of yet.
  void Forward();
  // After a call to the client: whether the fetch goes on, or was cancelled
  // under the call, which destroys it now.
  bool ReturnedFromClient();

  cppgc::Persistent<Resource> resource_;
  // The resource's client while this lives, but for one that the client
  // cancelled before the task.
  cppgc::Persistent<Observer> observer_;
  std::unique_ptr<FetchClient> client_;
  // Whether the task has come, after which the client hears what comes.
  bool delivering_ = false;
  bool response_started_ = false;
  // Whether the client is being called, and whether the fetch was cancelled
  // under the call.
  bool calling_client_ = false;
  bool cancel_pending_ = false;
  size_t body_sent_ = 0;
  base::WeakPtrFactory<PreloadedFetch> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_PRELOADED_FETCH_H_
