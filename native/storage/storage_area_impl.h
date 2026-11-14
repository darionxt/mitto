// One storage area in the Session, as Chrome's storage service keeps it: the
// map of an area of localStorage or sessionStorage, which every Page that has
// it open shares. It checks the quota, tells each observer of every change,
// the writer's too, and writes changes to the database in batches, limited as
// Chrome limits them. It keeps keys and values, as Chrome does on desktop, so
// it never asks a Page for an old value. It runs on the network thread.
//
// Upstream: components/services/storage/dom_storage/storage_area_impl.cc

#ifndef MITTO_STORAGE_STORAGE_AREA_IMPL_H_
#define MITTO_STORAGE_STORAGE_AREA_IMPL_H_

#include <stddef.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "mitto/page_messages.h"
#include "mitto/storage/local_storage_database.h"
#include "mitto/storage/storage_key.h"

namespace mitto {

// What a Page's area hears of the changes to it: Blink's StorageAreaObserver.
class StorageAreaObserver {
 public:
  virtual void KeyChanged(const std::string& key,
                          const std::string& new_value,
                          const std::optional<std::string>& old_value,
                          const PageStorageSource& source) = 0;
  virtual void KeyChangeFailed(const std::string& key,
                               const PageStorageSource& source) = 0;
  virtual void KeyDeleted(const std::string& key,
                          const std::optional<std::string>& old_value,
                          const PageStorageSource& source) = 0;
  virtual void AllDeleted(bool was_nonempty,
                          const PageStorageSource& source) = 0;

 protected:
  virtual ~StorageAreaObserver() = default;
};

class StorageAreaImpl {
 public:
  class Delegate {
   public:
    // No Page has the area open any more, and nothing waits for its map.
    virtual void OnNoBindings() = 0;
    // What to write of the area's usage with a commit.
    virtual void AddUsageToCommit(LocalStorageDatabase::MapUpdate& update) = 0;
    virtual void DidCommit(bool succeeded) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  struct Options {
    // The most bytes that its keys and values may hold, as Blink sends them.
    size_t max_size = 0;
    // How long to wait after a change before writing it, for others to come.
    base::TimeDelta default_commit_delay;
    // The most bytes to write in an hour, and the most commits.
    int max_bytes_per_hour = 0;
    int max_commits_per_hour = 0;
  };

  // `database` is null for an area that is never written, such as one of
  // sessionStorage, and outlives the area otherwise.
  StorageAreaImpl(base::SequenceBound<LocalStorageDatabase>* database,
                  StorageKey storage_key,
                  Delegate* delegate,
                  const Options& options);

  StorageAreaImpl(const StorageAreaImpl&) = delete;
  StorageAreaImpl& operator=(const StorageAreaImpl&) = delete;

  ~StorageAreaImpl();

  // Starts the area loaded and empty, with nothing to read from a database.
  void InitializeAsEmpty();

  // A Page binds the area, and lets it go. Each Bind() has its Unbind().
  void Bind();
  void Unbind();
  bool has_bindings() const { return bindings_ > 0; }

  // The bytes of its keys and values that count against the quota.
  size_t storage_used() const { return storage_used_; }
  bool empty() const { return storage_used_ == 0; }

  bool HasPendingCommit() const { return commit_batch_ != nullptr; }
  bool HasCommitsInFlight() const { return commit_batches_in_flight_ > 0; }
  bool has_pending_load_tasks() const {
    return !on_load_complete_tasks_.empty();
  }

  // Writes what has changed as soon as it can.
  void ScheduleImmediateCommit();

  // Blink's StorageArea. An observer stays until RemoveObserver(), and
  // outlives its time in the area.
  void AddObserver(StorageAreaObserver* observer);
  void RemoveObserver(StorageAreaObserver* observer);
  void Put(const std::string& key,
           const std::string& value,
           const PageStorageSource& source);
  void Delete(const std::string& key, const PageStorageSource& source);
  // `new_observer`, if there is one still when the change is made, hears of it
  // first.
  void DeleteAll(const PageStorageSource& source,
                 base::WeakPtr<StorageAreaObserver> new_observer);
  // Answers with what the area holds once it has loaded, and adds
  // `new_observer` right after, if it is still there.
  using GetAllCallback = base::OnceCallback<void(
      std::vector<std::pair<std::string, std::string>>)>;
  void GetAll(base::WeakPtr<StorageAreaObserver> new_observer,
              GetAllCallback callback);

  base::WeakPtr<StorageAreaImpl> GetWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

 private:
  // Limits the rate of commits, as Chrome's RateLimiter does.
  class RateLimiter {
   public:
    RateLimiter(size_t desired_rate, base::TimeDelta time_quantum);

    void add_samples(size_t samples) { samples_ += samples; }

    // How long to wait, `elapsed_time` after the area started, to have taken
    // the samples so far at the desired rate.
    base::TimeDelta ComputeDelayNeeded(base::TimeDelta elapsed_time) const;

   private:
    float rate_;
    float samples_ = 0;
    base::TimeDelta time_quantum_;
  };

  struct CommitBatch {
    CommitBatch();
    ~CommitBatch();

    bool clear_all_first = false;
    std::set<std::string> changed_keys;
  };

  enum class MapState {
    kUnloaded,
    kLoading,
    kLoaded,
  };

  enum class AccessMode {
    kReadOnly,
    kReadWrite,
  };

  struct OnLoadCompleteTask {
    OnLoadCompleteTask(base::OnceClosure callback, AccessMode mode);
    OnLoadCompleteTask(OnLoadCompleteTask&&);
    OnLoadCompleteTask& operator=(OnLoadCompleteTask&&);
    ~OnLoadCompleteTask();

    base::OnceClosure callback;
    AccessMode mode;
  };

  bool has_pending_load_read_write_tasks() const;

  void LoadMap(OnLoadCompleteTask task);
  void OnMapLoaded(std::optional<StorageValueMap> map);
  void OnLoadComplete();

  void CreateCommitBatchIfNeeded();
  void StartCommitTimer();
  base::TimeDelta ComputeCommitDelay() const;
  void CommitChanges();
  void OnCommitComplete(bool succeeded);

  void NotifyNoBindingsIfIdle();

  raw_ptr<base::SequenceBound<LocalStorageDatabase>> database_;
  const StorageKey storage_key_;
  const raw_ptr<Delegate> delegate_;

  MapState map_state_ = MapState::kUnloaded;
  StorageValueMap keys_values_map_;
  std::vector<OnLoadCompleteTask> on_load_complete_tasks_;
  std::set<raw_ptr<StorageAreaObserver>> observers_;
  int bindings_ = 0;

  size_t storage_used_ = 0;
  const size_t max_size_;
  const base::TimeTicks start_time_;
  const base::TimeDelta default_commit_delay_;
  RateLimiter data_rate_limiter_;
  RateLimiter commit_rate_limiter_;
  int commit_batches_in_flight_ = 0;
  std::unique_ptr<CommitBatch> commit_batch_;

  base::WeakPtrFactory<StorageAreaImpl> weak_ptr_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_STORAGE_STORAGE_AREA_IMPL_H_
