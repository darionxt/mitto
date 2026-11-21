// When a document's fetch may start, as Blink's ResourceLoadScheduler of a
// frame decides it: until the parser inserts the body, the scheduler is
// tight, and a request below kHigh starts only while fewer than two
// throttleable requests run, or a kMedium one while fewer than two kMedium
// ones do; then every request starts at once. Waiting requests start in
// order of priority, then of arrival. A Page's frame is never hidden, frozen
// or throttled, so the scheduler's other limits never apply.
//
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_load_scheduler.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_loader.cc

#ifndef MITTO_DOM_RESOURCE_LOAD_SCHEDULER_H_
#define MITTO_DOM_RESOURCE_LOAD_SCHEDULER_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <set>
#include <tuple>

#include "base/functional/callback.h"
#include "mitto/dom/fetcher.h"

namespace mitto {

class ResourceLoadScheduler {
 public:
  using ClientId = uint64_t;
  static constexpr ClientId kInvalidClientId = 0;

  // What a request lets the scheduler do with it.
  enum class ThrottleOption {
    // Waits for its turn: most of a document's fetches.
    kThrottleable,
    // Waits only while the frame is stopped, which a Page's never is: a
    // fetch that may run long, such as fetch()'s or a preload's of as=fetch.
    kStoppable,
    // Never waits: one of a URL that is not HTTP or HTTPS.
    kCanNotBeStoppedOrThrottled,
  };

  ResourceLoadScheduler();
  ResourceLoadScheduler(const ResourceLoadScheduler&) = delete;
  ResourceLoadScheduler& operator=(const ResourceLoadScheduler&) = delete;
  ~ResourceLoadScheduler();

  // GenerateClientId(): the id of a request to come, which releases it.
  ClientId GenerateClientId() { return next_id_++; }
  // Request(): `run` starts the fetch of `id`, now or once its turn comes.
  void Request(ClientId id,
               base::OnceClosure run,
               ThrottleOption option,
               FetchPriority priority);
  // Release(): the fetch has ended, or will never start, which lets another
  // start, at once if `schedule`.
  void Release(ClientId id, bool schedule);
  // LoosenThrottlingPolicy(): the body is there, so every request that
  // waits starts.
  void LoosenThrottlingPolicy();

 private:
  // ClientIdWithPriority::Compare: the higher priority first, then the
  // earlier request.
  using PendingKey = std::tuple<int, ClientId>;

  struct Pending {
    base::OnceClosure run;
    ThrottleOption option;
    FetchPriority priority;
  };

  void MaybeRun();
  bool GetNextPendingRequest(ClientId& id);
  void Run(ClientId id,
           base::OnceClosure run,
           bool throttleable,
           FetchPriority priority);
  size_t GetOutstandingLimit(FetchPriority priority) const;
  bool IsRunningThrottleableRequestsLessThanOutStandingLimit(
      size_t limit,
      FetchPriority priority) const;

  bool tight_ = true;
  ClientId next_id_ = 1;
  // The ids in each queue, best first, and what each waiting one runs.
  std::set<PendingKey> pending_throttleable_;
  std::map<ClientId, Pending> pending_;
  std::set<ClientId> running_;
  std::set<ClientId> running_throttleable_;
  std::set<ClientId> running_medium_;
};

}  // namespace mitto

#endif  // MITTO_DOM_RESOURCE_LOAD_SCHEDULER_H_
