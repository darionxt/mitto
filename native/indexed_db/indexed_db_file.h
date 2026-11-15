// The file of IndexedDB in a Session's storage directory: a SQLite database
// that keeps the ordered store of each storage key, as Chrome keeps each in a
// LevelDB of its own. Each row is one entry of one storage key's store, and a
// batch of writes goes into one transaction. It blocks, so it lives on a
// sequence that may.
//
// Chrome's LevelDB and the SQLite store it is moving to are files no page
// reads, so a Session keeps its own form, which only mitto reads.

#ifndef MITTO_INDEXED_DB_INDEXED_DB_FILE_H_
#define MITTO_INDEXED_DB_INDEXED_DB_FILE_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "mitto/indexed_db/kv_store.h"

namespace sql {
class Database;
}  // namespace sql

namespace mitto {

// The file's name in a Session's storage directory.
inline constexpr base::FilePath::CharType kIndexedDbFileName[] =
    FILE_PATH_LITERAL("IndexedDB");

class IndexedDbFile {
 public:
  IndexedDbFile();
  IndexedDbFile(const IndexedDbFile&) = delete;
  IndexedDbFile& operator=(const IndexedDbFile&) = delete;
  ~IndexedDbFile();

  // Opens the database at `path`, an absolute path, and makes its tables if it
  // is new. Returns false if it cannot, or if a later version wrote it.
  bool Open(const base::FilePath& path);

  // What the store of `storage_key`, as StorageKey::Serialize() writes it,
  // holds: nothing for one that is not there, or nullopt if it cannot be read.
  std::optional<KvEntries> Read(const std::string& storage_key);

  // How many bytes of keys and values the store of `storage_key` holds, or
  // nullopt if it cannot be read.
  std::optional<int64_t> Usage(const std::string& storage_key);

  // Writes `batch` to the store of `storage_key` in one transaction, and
  // returns whether it could, which writes none of it if not.
  bool Write(const std::string& storage_key, const KvBatch& batch);

 private:
  std::unique_ptr<sql::Database> database_;
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_INDEXED_DB_FILE_H_
