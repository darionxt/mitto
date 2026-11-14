// The quota of a Session's storage, as Chrome's QuotaManager enforces it and
// reports it: what IndexedDB and the Cache API hold for each storage key,
// which a write may not take past the key's quota, and what
// navigator.storage.estimate() says. DOM storage has a quota of its own and
// counts for none of it, as in Chrome.
//
// The quota enforced is Chrome's: on disk, 75% of a pool of 80% of the disk
// that holds the Session's storage; in memory, Incognito's, a third of a pool
// of a random 15% to 20% of the memory. What estimate() reports is the usage
// and 10 GiB more, on a disk of 10 GiB or more and always in memory, as
// kStaticStorageQuota has it.
//
// Upstream: storage/browser/quota/quota_settings.cc
// Upstream: storage/browser/quota/quota_manager_impl.cc

#ifndef MITTO_STORAGE_QUOTA_H_
#define MITTO_STORAGE_QUOTA_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"

namespace mitto {

class StorageQuota {
 public:
  // storage::QuotaClientType, of those a Page has.
  enum class ClientType {
    kIndexedDatabase,
    kServiceWorkerCache,
  };

  // What holds a part of the usage.
  class Client {
   public:
    virtual ClientType type() const = 0;
    // Calls `done` with how many bytes the client holds for `storage_key`.
    virtual void GetUsage(const std::string& storage_key,
                          base::OnceCallback<void(int64_t)> done) = 0;

   protected:
    virtual ~Client() = default;
  };

  // What estimate() gives, and the quota that writes are held to.
  struct UsageAndQuota {
    int64_t usage = 0;
    int64_t indexed_db = 0;
    int64_t caches = 0;
    // What a write may not go past.
    int64_t quota = 0;
    // What estimate() says the quota is.
    int64_t reported_quota = 0;
  };

  // `directory` is where the Session keeps its storage, whose disk sets the
  // quota, or nullopt for storage in memory, whose quota `physical_memory`
  // sets. `file_runner` reads the size of the disk.
  StorageQuota(std::optional<base::FilePath> directory,
               uint64_t physical_memory,
               scoped_refptr<base::SequencedTaskRunner> file_runner);
  StorageQuota(const StorageQuota&) = delete;
  StorageQuota& operator=(const StorageQuota&) = delete;
  ~StorageQuota();

  // Each client lives as long as this.
  void RegisterClient(Client& client);

  void GetUsageAndQuota(const std::string& storage_key,
                        base::OnceCallback<void(UsageAndQuota)> done);
  // How much the storage key may still write: its quota less its usage.
  void GetSpaceRemaining(const std::string& storage_key,
                         base::OnceCallback<void(std::optional<int64_t>)> done);
  // A client holds `delta` more bytes for `storage_key`.
  void NotifyModified(const std::string& storage_key,
                      ClientType type,
                      int64_t delta);

 private:
  struct Usage {
    int64_t indexed_db = 0;
    int64_t caches = 0;
  };
  struct KeyState;

  // Calls `done` once the quota and the key's usage are known.
  void WhenKnown(const std::string& storage_key, base::OnceClosure done);
  void DidGetTotalDiskSpace(std::optional<int64_t> total);
  void DidGetClientUsage(const std::string& storage_key,
                         ClientType type,
                         int64_t usage);
  UsageAndQuota Compute(const std::string& storage_key) const;

  const std::optional<base::FilePath> directory_;
  const scoped_refptr<base::SequencedTaskRunner> file_runner_;
  std::vector<raw_ptr<Client>> clients_;
  // The quota of each storage key, once it is known, and what estimate()
  // reports beside the usage.
  std::optional<int64_t> per_key_quota_;
  int64_t reported_quota_extra_ = 0;
  bool asked_disk_ = false;
  std::vector<base::OnceClosure> waiting_for_quota_;
  std::map<std::string, KeyState> keys_;
  base::WeakPtrFactory<StorageQuota> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_STORAGE_QUOTA_H_
