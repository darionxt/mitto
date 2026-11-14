#include "mitto/page/session_storage_backend.h"

#include <iterator>

#include "base/functional/bind.h"
#include "base/location.h"
#include "mitto/dom/cached_storage_area.h"
#include "mitto/dom/event_loop.h"

namespace mitto {

SessionStorageBackend::SessionStorageBackend() = default;
SessionStorageBackend::~SessionStorageBackend() = default;

void SessionStorageBackend::SetSend(SendFrame send) {
  send_ = std::move(send);
  // Blink's observers hear from the agent group's default task runner.
  main_thread_ = EventLoop::Get()
                     ? EventLoop::Get()->TaskRunner(TaskType::kInternalDefault)
                     : base::SingleThreadTaskRunner::GetCurrentDefault();
}

uint64_t SessionStorageBackend::NewObserver() {
  return ++last_observer_;
}

uint64_t SessionStorageBackend::Bind(PageStorageKind kind,
                                     uint64_t document,
                                     uint64_t observer,
                                     CachedStorageArea* area) {
  const uint64_t number = ++last_area_;
  Route(observer, number, area);
  send_.Run(EncodeStorageBind(document, {number, kind, observer}));
  return number;
}

std::vector<std::pair<std::string, std::string>> SessionStorageBackend::GetAll(
    uint64_t area,
    uint64_t observer) {
  const auto found = std::ranges::find_if(
      observers_, [area](const auto& o) { return o.second.first == area; });
  CHECK(found != observers_.end());
  Route(observer, area, found->second.second);
  {
    base::AutoLock lock(lock_);
    waiting_area_ = area;
    entries_.clear();
    complete_ = false;
  }
  send_.Run(EncodeStorageGetAll({area, observer}));
  base::AutoLock lock(lock_);
  // The process ends when the channel closes, so this cannot wait forever.
  while (!complete_) {
    answered_.Wait();
  }
  waiting_area_.reset();
  return std::move(entries_);
}

void SessionStorageBackend::Mutate(PageMessage type,
                                   PageStorageMutation mutation) {
  if (type == PageMessage::kStorageDeleteAll && mutation.observer != 0) {
    const auto found = std::ranges::find_if(
        observers_,
        [&mutation](const auto& o) { return o.second.first == mutation.area; });
    CHECK(found != observers_.end());
    Route(mutation.observer, mutation.area, found->second.second);
  }
  // A key and value within the quota fit, so only a URL longer than a frame
  // could keep a change in the Page, as an over-long Mojo message would.
  if (StorageMutationFits(mutation)) {
    send_.Run(EncodeStorageMutation(type, mutation));
  }
}

void SessionStorageBackend::Unbind(uint64_t area) {
  std::erase_if(observers_,
                [area](const auto& o) { return o.second.first == area; });
  send_.Run(EncodeStorageUnbind(area));
}

void SessionStorageBackend::Route(uint64_t observer,
                                  uint64_t area,
                                  CachedStorageArea* cached) {
  // An observer takes the place of the area's last.
  std::erase_if(observers_,
                [area](const auto& o) { return o.second.first == area; });
  observers_[observer] = {area, cached};
}

bool SessionStorageBackend::AnswerEntries(PageStorageEntries entries) {
  base::AutoLock lock(lock_);
  if (waiting_area_ != entries.area || complete_) {
    return false;
  }
  std::ranges::move(entries.entries, std::back_inserter(entries_));
  if (entries.last) {
    complete_ = true;
    answered_.Signal();
  }
  return true;
}

void SessionStorageBackend::OnEvent(PageMessage type, PageStorageEvent event) {
  main_thread_->PostTask(
      FROM_HERE,
      base::BindOnce(&SessionStorageBackend::OnEventOnMainThread,
                     base::Unretained(this), type, std::move(event)));
}

void SessionStorageBackend::OnEventOnMainThread(PageMessage type,
                                                const PageStorageEvent& event) {
  // An observer that another replaced, or whose area went, hears nothing.
  const auto found = observers_.find(event.observer);
  if (found == observers_.end()) {
    return;
  }
  found->second.second->OnEvent(type, event);
}

}  // namespace mitto
