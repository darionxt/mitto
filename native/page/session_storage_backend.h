// localStorage and sessionStorage through the Session, as Blink's
// CachedStorageArea talks to the browser's storage service. GetAll() asks over
// the channel and waits on the main thread for the answer, which the I/O
// thread hands over, as Blink waits in its synchronous call. Every other call
// goes without an answer, and the channel keeps the order. What the Session
// tells the areas' observers comes back to the main thread as tasks of their
// own, as Mojo messages do.
//
// Upstream: third_party/blink/renderer/modules/storage/cached_storage_area.cc

#ifndef MITTO_PAGE_SESSION_STORAGE_BACKEND_H_
#define MITTO_PAGE_SESSION_STORAGE_BACKEND_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "mitto/dom/storage_backend.h"
#include "mitto/page/page_reporter.h"

namespace mitto {

class SessionStorageBackend final : public StorageBackend {
 public:
  SessionStorageBackend();

  SessionStorageBackend(const SessionStorageBackend&) = delete;
  SessionStorageBackend& operator=(const SessionStorageBackend&) = delete;

  ~SessionStorageBackend() override;

  // On the main thread, before anything else: how frames go to the Session.
  void SetSend(SendFrame send);

  // StorageBackend, on the main thread:
  uint64_t NewObserver() override;
  uint64_t Bind(PageStorageKind kind,
                uint64_t document,
                uint64_t observer,
                CachedStorageArea* area) override;
  std::vector<std::pair<std::string, std::string>> GetAll(
      uint64_t area,
      uint64_t observer) override;
  void Mutate(PageMessage type, PageStorageMutation mutation) override;
  void Unbind(uint64_t area) override;

  // On the I/O thread: a frame of what the area that GetAll() waits for
  // holds. Returns false if none waits for that area, which the Page refuses.
  bool AnswerEntries(PageStorageEntries entries);
  // On the I/O thread: what the Session tells an observer.
  void OnEvent(PageMessage type, PageStorageEvent event);

 private:
  // On the main thread: what an observer's events go to.
  void Route(uint64_t observer, uint64_t area, CachedStorageArea* cached);
  void OnEventOnMainThread(PageMessage type, const PageStorageEvent& event);

  SendFrame send_;
  scoped_refptr<base::SingleThreadTaskRunner> main_thread_;
  // The main thread's: the areas' observers, each with its area's number and
  // the area it goes to, and the numbers last given.
  std::map<uint64_t, std::pair<uint64_t, raw_ptr<CachedStorageArea>>>
      observers_;
  uint64_t last_area_ = 0;
  uint64_t last_observer_ = 0;

  base::Lock lock_;
  base::ConditionVariable answered_{&lock_};
  // The area that GetAll() waits for, if it does, what has come of it, and
  // whether all of it has.
  std::optional<uint64_t> waiting_area_ GUARDED_BY(lock_);
  std::vector<std::pair<std::string, std::string>> entries_ GUARDED_BY(lock_);
  bool complete_ GUARDED_BY(lock_) = false;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_STORAGE_BACKEND_H_
