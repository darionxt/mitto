#include "mitto/dom/resource_load_scheduler.h"

#include <utility>

namespace mitto {
namespace {

// The field trial's defaults, which stable has.
constexpr size_t kTightLimit = 2;
constexpr size_t kTightMediumLimit = 2;
constexpr size_t kNormalLimit = 1024;

}  // namespace

ResourceLoadScheduler::ResourceLoadScheduler() = default;

ResourceLoadScheduler::~ResourceLoadScheduler() = default;

void ResourceLoadScheduler::Request(ClientId id,
                                    base::OnceClosure run,
                                    ThrottleOption option,
                                    FetchPriority priority) {
  // IsClientDelayable(): only a throttleable request waits in a frame that
  // is not stopped.
  if (option != ThrottleOption::kThrottleable) {
    Run(id, std::move(run), /*throttleable=*/false, priority);
    return;
  }
  pending_throttleable_.insert({-static_cast<int>(priority), id});
  pending_.emplace(id, Pending{std::move(run), option, priority});
  MaybeRun();
}

void ResourceLoadScheduler::Release(ClientId id, bool schedule) {
  if (id == kInvalidClientId) {
    return;
  }
  if (running_.erase(id)) {
    running_throttleable_.erase(id);
    running_medium_.erase(id);
  } else if (const auto it = pending_.find(id); it != pending_.end()) {
    pending_throttleable_.erase({-static_cast<int>(it->second.priority), id});
    pending_.erase(it);
  } else {
    return;
  }
  if (schedule) {
    MaybeRun();
  }
}

void ResourceLoadScheduler::LoosenThrottlingPolicy() {
  if (!tight_) {
    return;
  }
  tight_ = false;
  MaybeRun();
}

void ResourceLoadScheduler::MaybeRun() {
  ClientId id = kInvalidClientId;
  while (GetNextPendingRequest(id)) {
    const auto it = pending_.find(id);
    if (it == pending_.end()) {
      continue;
    }
    Pending pending = std::move(it->second);
    pending_.erase(it);
    Run(id, std::move(pending.run),
        pending.option == ThrottleOption::kThrottleable, pending.priority);
  }
}

bool ResourceLoadScheduler::GetNextPendingRequest(ClientId& id) {
  // A Page's frame is never stopped, so no stoppable request waits; only
  // the best throttleable one may start.
  if (pending_throttleable_.empty()) {
    return false;
  }
  const auto best = pending_throttleable_.begin();
  const auto found = pending_.find(std::get<1>(*best));
  const FetchPriority priority =
      found != pending_.end() ? found->second.priority : FetchPriority::kLow;
  if (!IsRunningThrottleableRequestsLessThanOutStandingLimit(
          GetOutstandingLimit(priority), priority)) {
    return false;
  }
  id = std::get<1>(*best);
  pending_throttleable_.erase(best);
  return true;
}

void ResourceLoadScheduler::Run(ClientId id,
                                base::OnceClosure run,
                                bool throttleable,
                                FetchPriority priority) {
  running_.insert(id);
  if (throttleable) {
    running_throttleable_.insert(id);
  }
  if (priority == FetchPriority::kMedium) {
    running_medium_.insert(id);
  }
  std::move(run).Run();
}

size_t ResourceLoadScheduler::GetOutstandingLimit(
    FetchPriority priority) const {
  if (tight_ && priority < FetchPriority::kHigh) {
    return kTightLimit;
  }
  return kNormalLimit;
}

bool ResourceLoadScheduler::
    IsRunningThrottleableRequestsLessThanOutStandingLimit(
        size_t limit,
        FetchPriority priority) const {
  // A few kMedium requests run whatever else runs.
  if (priority == FetchPriority::kMedium &&
      running_medium_.size() < kTightMediumLimit) {
    return true;
  }
  return running_throttleable_.size() < limit;
}

}  // namespace mitto
