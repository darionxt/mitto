// What a page's IndexedDB objects know of a database, its object stores and
// their indexes, as Blink's renderer keeps it: an object store's metadata and
// an index's are shared, so that what an IDBObjectStore changes, such as its
// name, the database's copy sees, and an aborted upgrade puts back the copies
// that it kept. They form a tree, database to object store to index, so
// sharing them by reference counting makes no cycle.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_metadata.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_metadata.cc

#ifndef MITTO_DOM_IDB_METADATA_H_
#define MITTO_DOM_IDB_METADATA_H_

#include <stdint.h>

#include <map>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"

namespace mitto {

class IDBIndexMetadata final : public base::RefCounted<IDBIndexMetadata> {
 public:
  static constexpr int64_t kInvalidId = -1;

  IDBIndexMetadata();
  IDBIndexMetadata(const std::u16string& name,
                   int64_t id,
                   const IDBKeyPath& key_path,
                   bool unique,
                   bool multi_entry);

  // The metadata as the messages carry it, and back.
  static scoped_refptr<IDBIndexMetadata> FromMessage(
      const IndexedDbIndexMetadata& metadata);
  IndexedDbIndexMetadata ToMessage() const;

  std::u16string name;
  int64_t id = kInvalidId;
  IDBKeyPath key_path;
  bool unique = false;
  bool multi_entry = false;

 private:
  friend class base::RefCounted<IDBIndexMetadata>;
  ~IDBIndexMetadata();
};

class IDBObjectStoreMetadata final
    : public base::RefCounted<IDBObjectStoreMetadata> {
 public:
  static constexpr int64_t kInvalidId = -1;

  IDBObjectStoreMetadata();
  IDBObjectStoreMetadata(const std::u16string& name,
                         int64_t id,
                         const IDBKeyPath& key_path,
                         bool auto_increment);

  static scoped_refptr<IDBObjectStoreMetadata> FromMessage(
      const IndexedDbObjectStoreMetadata& metadata);

  // A deep copy, with copies of its indexes' metadata.
  scoped_refptr<IDBObjectStoreMetadata> CreateCopy() const;

  std::u16string name;
  int64_t id = kInvalidId;
  IDBKeyPath key_path;
  bool auto_increment = false;
  int64_t max_index_id = 0;
  std::map<int64_t, scoped_refptr<IDBIndexMetadata>> indexes;

 private:
  friend class base::RefCounted<IDBObjectStoreMetadata>;
  ~IDBObjectStoreMetadata();
};

struct IDBDatabaseMetadata {
  // No version yet, and the version that a new database has before its
  // first upgrade.
  static constexpr int64_t kNoVersion = IndexedDbDatabaseMetadata::kNoVersion;
  static constexpr int64_t kDefaultVersion =
      IndexedDbDatabaseMetadata::kDefaultVersion;

  IDBDatabaseMetadata();
  IDBDatabaseMetadata(const IDBDatabaseMetadata&);
  IDBDatabaseMetadata& operator=(const IDBDatabaseMetadata&);
  ~IDBDatabaseMetadata();

  static IDBDatabaseMetadata FromMessage(
      const IndexedDbDatabaseMetadata& metadata);

  // Takes the database's own metadata from `metadata`, and leaves the object
  // stores' as they are.
  void CopyFrom(const IDBDatabaseMetadata& metadata);

  std::u16string name;
  int64_t version = kNoVersion;
  int64_t max_object_store_id = 0;
  std::map<int64_t, scoped_refptr<IDBObjectStoreMetadata>> object_stores;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_METADATA_H_
