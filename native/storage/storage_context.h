// A Session's storage, which its Pages share, as a profile's storage
// partition holds it in Chrome: where it is kept, the sequence that reads and
// writes it, and localStorage's areas. A Session with a storage directory
// keeps localStorage in the file LocalStorage there; one without keeps it in
// memory, as Chrome's Incognito does. sessionStorage is never written: each
// Page has a namespace of its own, which goes with it.
//
// IndexedDB and the Cache API hang off the same context, under the same
// directory.
//
// It lives on the network thread, from the Session's start to its end.
//
// Upstream: components/services/storage/dom_storage/local_storage_impl.cc
// Upstream: components/services/storage/dom_storage/session_storage_impl.cc

#ifndef MITTO_STORAGE_STORAGE_CONTEXT_H_
#define MITTO_STORAGE_STORAGE_CONTEXT_H_

#include <map>
#include <memory>
#include <optional>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "mitto/directory_lock.h"
#include "mitto/storage/local_storage_database.h"
#include "mitto/storage/storage_area_impl.h"
#include "mitto/storage/storage_key.h"

namespace mitto {

// What the Session allows beyond an area's quota, kPerStorageAreaQuota,
// counted in the bytes that Blink sends, for writes of several windows that
// race.
inline constexpr size_t kPerStorageAreaOverQuotaAllowance = 100 * 1024;

// The file of localStorage in a Session's storage directory, as Chrome names
// its SQLite database in a profile.
inline constexpr base::FilePath::CharType kLocalStorageFileName[] =
    FILE_PATH_LITERAL("LocalStorage");

// The sessionStorage of one Page: its areas, by storage key, for as long as the
// Page lasts, whether or not its documents still use them.
class SessionStorageNamespace {
 public:
  SessionStorageNamespace();

  SessionStorageNamespace(const SessionStorageNamespace&) = delete;
  SessionStorageNamespace& operator=(const SessionStorageNamespace&) = delete;

  ~SessionStorageNamespace();

  // The area of `storage_key`, which the namespace keeps until it goes.
  StorageAreaImpl& Area(const StorageKey& storage_key);

 private:
  class Holder;

  std::map<StorageKey, std::unique_ptr<Holder>> areas_;
};

class StorageContext {
 public:
  // `directory`, if any, is an absolute path that exists, and `lock` holds it.
  StorageContext(std::optional<base::FilePath> directory, DirectoryLock lock);

  StorageContext(const StorageContext&) = delete;
  StorageContext& operator=(const StorageContext&) = delete;

  ~StorageContext();

  // Opens localStorage's database, and says whether it could. Called once,
  // before anything else.
  void Open(base::OnceCallback<void(bool)> opened);

  // Runs `closed` on another sequence once the context has gone, all that was
  // left to write is written and its files are closed, so that the directory
  // can go to another Session. Every Page has unbound its areas by then.
  void WhenClosed(base::OnceClosure closed) { closed_ = std::move(closed); }

  // The Session's storage directory, or nullopt for one that keeps its storage
  // in memory.
  const std::optional<base::FilePath>& directory() const { return directory_; }
  bool in_memory() const { return !directory_.has_value(); }

  // A sequence that may block, for reading and writing the stores.
  const scoped_refptr<base::SequencedTaskRunner>& blocking_task_runner() const {
    return blocking_task_runner_;
  }

  // The area of localStorage of `storage_key`, which a Page binds at once with
  // StorageAreaImpl::Bind(). It stays for as long as one has it bound.
  StorageAreaImpl& LocalStorageArea(const StorageKey& storage_key);

 private:
  class LocalAreaHolder;

  // An area that no Page has open and that has nothing left to write goes, so
  // that its map leaves memory. It loads again when a Page opens it.
  void PurgeIfUnused(const StorageKey& storage_key);

  const std::optional<base::FilePath> directory_;
  DirectoryLock lock_;
  const scoped_refptr<base::SequencedTaskRunner> blocking_task_runner_;
  base::SequenceBound<LocalStorageDatabase> local_storage_database_;
  std::map<StorageKey, std::unique_ptr<LocalAreaHolder>> local_areas_;
  base::OnceClosure closed_;
  base::WeakPtrFactory<StorageContext> weak_ptr_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_STORAGE_STORAGE_CONTEXT_H_
