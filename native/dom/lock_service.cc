#include "mitto/dom/lock_service.h"

#include <iterator>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/event_loop.h"

namespace mitto {
namespace {

void Post(base::OnceClosure task) {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(TaskType::kWebLocks)
      ->PostTask(FROM_HERE, std::move(task));
}

}  // namespace

LockService::Lock::Lock(scoped_refptr<Client> client,
                        std::string client_id,
                        std::string name,
                        Mode mode,
                        int64_t lock_id)
    : client(std::move(client)),
      client_id(std::move(client_id)),
      name(std::move(name)),
      mode(mode),
      lock_id(lock_id) {}

LockService::Lock::Lock(Lock&&) = default;
LockService::Lock& LockService::Lock::operator=(Lock&&) = default;
LockService::Lock::~Lock() = default;

// LockManager::LockGroupState: the request queues of one lock group.
class LockService::LockGroupState {
 public:
  using Queue = std::list<Lock>;

  // Breaks the lock at `it`, which tells its client, and returns the next.
  Queue::iterator BreakLock(Queue& request_queue, Queue::iterator it) {
    lock_id_to_iterator_.erase(it->lock_id);
    Post(base::BindOnce(&Client::Broken, it->client, it->lock_id));
    return request_queue.erase(it);
  }

  // Steals the lock: breaks the locks that hold it, and grants the request
  // first.
  void PreemptLock(Lock lock) {
    // Preempting shared locks is not supported.
    CHECK(lock.mode == Mode::kExclusive);
    Queue& request_queue = resource_names_to_requests_[lock.name];
    while (!request_queue.empty() && request_queue.front().granted) {
      BreakLock(request_queue, request_queue.begin());
    }
    request_queue.push_front(std::move(lock));
    const auto it = request_queue.begin();
    lock_id_to_iterator_.emplace(it->lock_id, it);
    Grant(*it);
  }

  void AddRequest(Lock lock, WaitMode wait) {
    CHECK(wait != WaitMode::kPreempt);
    Queue& request_queue = resource_names_to_requests_[lock.name];
    // No holder is in the back/forward cache, whose contention would evict
    // it.
    const bool can_grant =
        request_queue.empty() || (request_queue.back().granted &&
                                  request_queue.back().mode == Mode::kShared &&
                                  lock.mode == Mode::kShared);
    if (!can_grant && wait == WaitMode::kNoWait) {
      Post(base::BindOnce(&Client::Failed, lock.client, lock.lock_id));
      if (request_queue.empty()) {
        resource_names_to_requests_.erase(lock.name);
      }
      return;
    }
    request_queue.push_back(std::move(lock));
    const auto it = std::prev(request_queue.end());
    lock_id_to_iterator_.emplace(it->lock_id, it);
    if (can_grant) {
      Grant(*it);
    }
  }

  void EraseLock(int64_t lock_id) {
    const auto iterator_it = lock_id_to_iterator_.find(lock_id);
    if (iterator_it == lock_id_to_iterator_.end()) {
      return;
    }
    const auto lock_it = iterator_it->second;
    lock_id_to_iterator_.erase(iterator_it);
    const auto request_it = resource_names_to_requests_.find(lock_it->name);
    if (request_it == resource_names_to_requests_.end()) {
      return;
    }
    Queue& request_queue = request_it->second;
    request_queue.erase(lock_it);
    if (request_queue.empty()) {
      resource_names_to_requests_.erase(request_it);
      return;
    }
    // If the front is not granted, the only granted lock has just gone: grant
    // the next exclusive request, or the shared ones at the front.
    if (request_queue.front().granted) {
      return;
    }
    if (request_queue.front().mode == Mode::kExclusive) {
      Grant(request_queue.front());
      return;
    }
    for (auto grantee = request_queue.begin();
         grantee != request_queue.end() && grantee->mode == Mode::kShared;
         ++grantee) {
      CHECK(!grantee->granted);
      Grant(*grantee);
    }
  }

  bool IsEmpty() const { return lock_id_to_iterator_.empty(); }

  std::pair<std::vector<Info>, std::vector<Info>> Snapshot() const {
    std::vector<Info> requests;
    std::vector<Info> held;
    for (const auto& [name, request_queue] : resource_names_to_requests_) {
      for (const Lock& lock : request_queue) {
        (lock.granted ? held : requests)
            .push_back(Info{lock.name, lock.mode, lock.client_id});
      }
    }
    return {std::move(requests), std::move(held)};
  }

 private:
  static void Grant(Lock& lock) {
    CHECK(!lock.granted);
    lock.granted = true;
    Post(base::BindOnce(&Client::Granted, lock.client, lock.lock_id));
  }

  // By name, in the order of their bytes, as base::flat_map keeps them.
  std::map<std::string, Queue> resource_names_to_requests_;
  std::map<int64_t, Queue::iterator> lock_id_to_iterator_;
};

// static
LockService& LockService::Get() {
  static base::NoDestructor<LockService> service;
  return *service;
}

LockService::LockService() = default;
LockService::~LockService() = default;

int64_t LockService::RequestLock(scoped_refptr<Client> client,
                                 std::string origin,
                                 std::string client_id,
                                 std::string name,
                                 Mode mode,
                                 WaitMode wait) {
  const int64_t lock_id = ++next_lock_id_;
  Post(base::BindOnce(&LockService::DoRequestLock, base::Unretained(this),
                      std::move(origin),
                      Lock(std::move(client), std::move(client_id),
                           std::move(name), mode, lock_id),
                      wait));
  return lock_id;
}

void LockService::ReleaseLock(std::string origin, int64_t lock_id) {
  Post(base::BindOnce(&LockService::DoReleaseLock, base::Unretained(this),
                      std::move(origin), lock_id));
}

void LockService::QueryState(
    std::string origin,
    base::OnceCallback<void(std::vector<Info>, std::vector<Info>)> answer) {
  Post(base::BindOnce(
      [](LockService* service, const std::string& origin,
         base::OnceCallback<void(std::vector<Info>, std::vector<Info>)>
             answer) {
        auto [pending, held] = service->Snapshot(origin);
        Post(base::BindOnce(std::move(answer), std::move(pending),
                            std::move(held)));
      },
      base::Unretained(this), std::move(origin), std::move(answer)));
}

// LockManager::RequestLock(), once the request has reached the browser.
void LockService::DoRequestLock(std::string origin, Lock lock, WaitMode wait) {
  if (wait == WaitMode::kPreempt) {
    CHECK_EQ(lock.mode, Mode::kExclusive);
  }
  std::unique_ptr<LockGroupState>& state = lock_groups_[origin];
  if (!state) {
    state = std::make_unique<LockGroupState>();
  }
  if (wait == WaitMode::kPreempt) {
    state->PreemptLock(std::move(lock));
  } else {
    state->AddRequest(std::move(lock), wait);
  }
  if (state->IsEmpty()) {
    lock_groups_.erase(origin);
  }
}

void LockService::DoReleaseLock(std::string origin, int64_t lock_id) {
  const auto it = lock_groups_.find(origin);
  if (it == lock_groups_.end()) {
    return;
  }
  it->second->EraseLock(lock_id);
  if (it->second->IsEmpty()) {
    lock_groups_.erase(it);
  }
}

std::pair<std::vector<LockService::Info>, std::vector<LockService::Info>>
LockService::Snapshot(const std::string& origin) const {
  const auto it = lock_groups_.find(origin);
  if (it == lock_groups_.end()) {
    return {};
  }
  return it->second->Snapshot();
}

}  // namespace mitto
