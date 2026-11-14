// The store of localStorage: the SQLite database that Chrome's storage service
// writes when kDomStorageSqlite is on, with its schema and its compression of
// values, in a file of the Session's storage directory, or in memory as
// Chrome's Incognito keeps it. It blocks, so it lives on a sequence that may.
//
// Upstream: components/services/storage/dom_storage/sqlite/local_storage_sqlite.cc
// Upstream: components/services/storage/dom_storage/sqlite/map_entries_table.cc
// Upstream: components/services/storage/dom_storage/sqlite/sqlite_database_utils.cc
// Upstream: components/services/storage/public/cpp/compression.cc

#ifndef MITTO_STORAGE_LOCAL_STORAGE_DATABASE_H_
#define MITTO_STORAGE_LOCAL_STORAGE_DATABASE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "mitto/storage/storage_key.h"

namespace sql {
class Database;
}  // namespace sql

namespace mitto {

// An area's keys and values, as Blink sends them, in the order of their bytes.
using StorageValueMap = std::map<std::string, std::string>;

class LocalStorageDatabase {
 public:
  // What to write of one area: Chrome's DomStorageDatabase::MapBatchUpdate.
  struct MapUpdate {
    explicit MapUpdate(StorageKey storage_key);
    MapUpdate(MapUpdate&&);
    MapUpdate& operator=(MapUpdate&&);
    ~MapUpdate();

    StorageKey storage_key;
    // Whether to delete every entry before the rest.
    bool clear_all_first = false;
    std::vector<std::pair<std::string, std::string>> entries_to_add;
    std::vector<std::string> keys_to_delete;
    // An area left empty loses its row of usage. Otherwise its row says when
    // it changed, its size, and the first time, when it was opened.
    bool delete_usage = false;
    std::optional<base::Time> last_accessed;
    std::optional<base::Time> last_modified;
    std::optional<uint64_t> total_size;
  };

  LocalStorageDatabase();

  LocalStorageDatabase(const LocalStorageDatabase&) = delete;
  LocalStorageDatabase& operator=(const LocalStorageDatabase&) = delete;

  ~LocalStorageDatabase();

  // Opens the database at `path`, an absolute path, and makes its tables if it
  // is new, or one in memory for an empty path. Returns false if it cannot,
  // or if a later Chrome wrote it.
  bool Open(const base::FilePath& path);

  // What the area of `storage_key` holds, which is nothing for one that is not
  // there, or nullopt if it cannot be read.
  std::optional<StorageValueMap> ReadMap(const StorageKey& storage_key);

  // Writes `updates` in one transaction. Returns false if it fails, which
  // writes none of them.
  bool UpdateMaps(std::vector<MapUpdate> updates);

  // Writes when each area was last opened, which Chrome writes when an area
  // closes.
  bool PutLastAccessed(const std::vector<StorageKey>& storage_keys,
                       base::Time last_accessed);

 private:
  std::optional<std::optional<int64_t>> FindMapId(const StorageKey& key);
  bool PutMapMetadata(const StorageKey& key,
                      std::optional<base::Time> last_accessed,
                      std::optional<base::Time> last_modified,
                      std::optional<uint64_t> total_size);

  std::unique_ptr<sql::Database> database_;
};

}  // namespace mitto

#endif  // MITTO_STORAGE_LOCAL_STORAGE_DATABASE_H_
