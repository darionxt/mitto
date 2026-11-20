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

class Resource;
class WindowTasks;

class PreloadedFetch final : public FetchController {
 public:
  // Starts `client` on `resource`, which it hears of from a task of `tasks`.
  // It owns itself until the client's end, or until the window drops the
  // task.
  static void Start(Resource& resource,
                    std::unique_ptr<FetchClient> client,
                    WindowTasks& tasks);

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
  void NotifyFinished();
  // Tells the client of the response, once it has started, and of the body
  // that it has not heard of yet.
  void Forward();
  // Stops being the resource's client, and destroys this.
  void End();

  cppgc::Persistent<Resource> resource_;
  cppgc::Persistent<Observer> observer_;
  std::unique_ptr<FetchClient> client_;
  bool response_started_ = false;
  size_t body_sent_ = 0;
  base::WeakPtrFactory<PreloadedFetch> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_DOM_PRELOADED_FETCH_H_
