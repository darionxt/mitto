// What IndexedDB keeps for one storage key, as Chrome's on-disk store keeps
// it: every database of the key in one ordered store, each object store's
// records by key, each index's entries by index key and then primary key, and
// the metadata. A transaction's writes stay with it until it commits them,
// all at once, so that transactions on other object stores run beside it and
// one that aborts leaves nothing behind, as with LevelDB.
//
// Keys are laid out so that the store's byte order is the order of IndexedDB
// keys: a database's entries under its number, an object store's under its
// ID, and an IndexedDB key in Chrome's sortable encoding, which no other
// encoding is a prefix of.
//
// Upstream: content/browser/indexed_db/instance/backing_store.h
// Upstream: content/browser/indexed_db/instance/leveldb/backing_store.cc

#ifndef MITTO_INDEXED_DB_BACKING_STORE_H_
#define MITTO_INDEXED_DB_BACKING_STORE_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ref.h"
#include "mitto/indexed_db/kv_store.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"

namespace mitto {

class IndexedDbBackingStore {
 public:
  class Transaction;

  // An open cursor over an object store or an index, within a range, in a
  // direction.
  class Cursor {
   public:
    // Where a cursor is, which it can go back to.
    struct Position {
      std::string kv_key;
      IDBKey key;
      IDBKey primary_key;
    };

    ~Cursor();

    const IDBKey& key() const { return key_; }
    const IDBKey& primary_key() const { return primary_key_; }
    // The record's value: empty for a cursor of keys only.
    const std::string& value() const { return value_; }

    // Moves to the next entry, or to the next at or past `key` and
    // `primary_key` when they are valid, and returns whether there is one.
    bool Continue(const IDBKey& key, const IDBKey& primary_key);
    bool Continue() { return Continue(IDBKey(), IDBKey()); }
    // Moves on `count` entries, and returns whether there is one there.
    bool Advance(uint32_t count);

    Position GetPosition() const { return {position_, key_, primary_key_}; }
    // Goes back to `position`, without its value, which the Page has.
    void SetPosition(Position position);

   private:
    friend class Transaction;

    Cursor(Transaction& transaction,
           int64_t object_store_id,
           int64_t index_id,
           std::string lower,
           std::string upper,
           IndexedDbCursorDirection direction,
           bool key_only);

    bool IsIndex() const {
      return index_id_ != IndexedDbIndexMetadata::kInvalidId;
    }
    bool IsReverse() const;
    bool IsUnique() const;
    // Finds the first entry in the direction from `from`, inclusive, and
    // takes it, and returns whether there is one.
    bool FirstFrom(const std::string& from);
    // Takes the entry of KV key `kv_key`.
    bool Take(const std::string& kv_key);
    // For prevunique, the first entry of the index key that `kv_key` has.
    std::string FirstOfIndexKey(const std::string& kv_key);

    const raw_ref<Transaction> transaction_;
    const int64_t object_store_id_;
    const int64_t index_id_;
    std::string lower_;
    std::string upper_;
    const IndexedDbCursorDirection direction_;
    const bool key_only_;
    std::string position_;
    IDBKey key_;
    IDBKey primary_key_;
    std::string value_;
  };

  class Transaction {
   public:
    Transaction(IndexedDbBackingStore& store, int64_t database_id);
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    // Writes what the transaction wrote, and returns whether it could.
    bool Commit();
    void Rollback();
    // What the commit adds to the store's size, at most: the bytes it
    // writes.
    int64_t WriteSize() const;

    bool SetDatabaseVersion(int64_t version, int64_t max_object_store_id);
    bool CreateObjectStore(const IndexedDbObjectStoreMetadata& store);
    bool DeleteObjectStore(int64_t object_store_id);
    bool UpdateObjectStore(const IndexedDbObjectStoreMetadata& store);
    bool CreateIndex(int64_t object_store_id,
                     const IndexedDbIndexMetadata& index);
    bool DeleteIndex(int64_t object_store_id, int64_t index_id);

    std::optional<std::string> GetRecord(int64_t object_store_id,
                                         const IDBKey& key);
    bool KeyExistsInObjectStore(int64_t object_store_id, const IDBKey& key);
    // Writes the record and replaces the index entries it had.
    void PutRecord(int64_t object_store_id,
                   const IDBKey& key,
                   const std::string& value);
    // Adds index entries of a record, as an index writer does.
    void PutIndexEntries(int64_t object_store_id,
                         int64_t index_id,
                         const IDBKey& primary_key,
                         const std::vector<IDBKey>& index_keys);
    // Removes the records in `range` and their index entries, and returns
    // how many it removed.
    uint32_t DeleteRange(int64_t object_store_id,
                         const IndexedDbKeyRange& range);
    void ClearObjectStore(int64_t object_store_id);
    // Whether an entry of the index has `index_key` with a primary key other
    // than `primary_key`.
    bool IndexKeyExistsWithOtherPrimaryKey(int64_t object_store_id,
                                           int64_t index_id,
                                           const IDBKey& index_key,
                                           const IDBKey& primary_key);

    int64_t GetKeyGeneratorCurrentNumber(int64_t object_store_id);
    // Raises the key generator to `new_number` if it is lower, or sets it
    // when `was_generated`.
    void MaybeUpdateKeyGeneratorCurrentNumber(int64_t object_store_id,
                                              int64_t new_number,
                                              bool was_generated);

    // A cursor at the first entry, or null if there is none.
    std::unique_ptr<Cursor> OpenCursor(int64_t object_store_id,
                                       int64_t index_id,
                                       const IndexedDbKeyRange& range,
                                       IndexedDbCursorDirection direction,
                                       bool key_only);
    uint32_t Count(int64_t object_store_id,
                   int64_t index_id,
                   const IndexedDbKeyRange& range);

    // The store's entries, with what this has written over them.
    std::optional<std::string> Get(const std::string& key);
    std::optional<KvStore::Entry> Seek(const std::string& key,
                                       const std::string& lower,
                                       const std::string& upper,
                                       bool reverse);

   private:
    void Put(std::string key, std::string value);
    void Remove(std::string key);
    // Removes every entry in [lower, upper).
    void RemoveRange(const std::string& lower, const std::string& upper);
    // Removes the index entries of a record, from its back references.
    void RemoveIndexEntries(int64_t object_store_id, const IDBKey& key);

    const raw_ref<IndexedDbBackingStore> store_;
    const int64_t database_id_;
    KvBatch writes_;
  };

  explicit IndexedDbBackingStore(std::unique_ptr<KvStore> kv);
  IndexedDbBackingStore(const IndexedDbBackingStore&) = delete;
  IndexedDbBackingStore& operator=(const IndexedDbBackingStore&) = delete;
  ~IndexedDbBackingStore();

  // The databases' names and versions, in the order of their names.
  std::vector<std::pair<std::u16string, int64_t>> GetDatabaseNamesAndVersions();
  // The number and metadata of the database of `name`, or nullopt.
  std::optional<std::pair<int64_t, IndexedDbDatabaseMetadata>> LoadDatabase(
      const std::u16string& name);
  // Makes a database with no version, and returns its number.
  std::optional<int64_t> CreateDatabase(const std::u16string& name);
  bool DeleteDatabase(int64_t database_id, const std::u16string& name);
  // What it holds, as the quota counts it.
  int64_t Size() const { return kv_->Size(); }

 private:
  std::unique_ptr<KvStore> kv_;
};

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_BACKING_STORE_H_
