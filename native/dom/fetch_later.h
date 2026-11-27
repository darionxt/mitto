// The Fetch Standard's fetchLater(): a keepalive fetch that the Session holds
// until the page's timer asks for it, its document goes, the back/forward
// cache takes it or its Page ends, within the quota of deferred bytes that
// its frame's deferred-fetch control document has, as Blink's
// FetchLaterManager queues one with the browser's FetchLaterLoader.
//
// Upstream: third_party/blink/renderer/core/fetch/fetch_manager.cc
// Upstream: third_party/blink/renderer/core/fetch/fetch_later_util.cc

#ifndef MITTO_DOM_FETCH_LATER_H_
#define MITTO_DOM_FETCH_LATER_H_

#include <stdint.h>

#include <map>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/dom/html_frame_owner_element.h"
#include "url/origin.h"

namespace mitto {

class FetchLaterResult;
class Frame;
class Window;

// The deferred requests of a window that have not gone yet, by what each
// counts against the quota, as Blink's FetchLaterManager keeps its
// DeferredLoaders. A request's client holds a reference, so it outlives the
// window as long as one is still there.
class DeferredFetches : public base::RefCounted<DeferredFetches> {
 public:
  DeferredFetches();

  DeferredFetches(const DeferredFetches&) = delete;
  DeferredFetches& operator=(const DeferredFetches&) = delete;

  // Counts `bytes` of a request to `origin`, which `send` sends now, and
  // returns its ID.
  uint64_t Add(const url::Origin& origin,
               uint64_t bytes,
               base::OnceClosure send);
  void Remove(uint64_t id);
  // FetchLaterManager::UpdateDeferredBytesQuota(): takes what each request
  // counts from `total`, and what each to `origin` counts from `for_origin`.
  void TakeFromQuota(const url::Origin& origin,
                     uint64_t& for_origin,
                     uint64_t& total) const;
  // FetchLaterManager::ContextEnteredBackForwardCache(): each request goes now,
  // as Chrome 157's default send_on_enter_bfcache has it.
  void SendAll();

 private:
  friend class base::RefCounted<DeferredFetches>;
  ~DeferredFetches();

  struct Record {
    Record();
    Record(Record&&);
    Record& operator=(Record&&);
    ~Record();

    url::Origin origin;
    uint64_t bytes = 0;
    base::OnceClosure send;
  };
  std::map<uint64_t, Record> records_;
  uint64_t next_id_ = 1;
};

// FetchLaterUtil::GetContainerDeferredFetchPolicyOnNavigation(): what the
// element of `frame` reserves as the frame's parent navigates it to a
// document of `to_origin`.
DeferredFetchPolicy DeferredFetchPolicyOnNavigation(
    const Frame& frame,
    const url::Origin& to_origin);

// fetchLater(input, init) of `window`.
FetchLaterResult* FetchLater(Window& window,
                             const bindings::RequestOrUSVString& input,
                             const bindings::DeferredRequestInit& init,
                             bindings::ExceptionState& exception_state);

}  // namespace mitto

#endif  // MITTO_DOM_FETCH_LATER_H_
