// The messages of IndexedDB between a Page and its Session: what the Page's
// IDBFactory, IDBDatabase, IDBTransaction and IDBCursor objects ask, as Blink
// asks Chrome's browser over blink.mojom.IDBFactory, IDBDatabase,
// IDBTransaction and IDBCursor, and what the Session answers, as the browser
// calls IDBFactoryClient and IDBDatabaseCallbacks and answers each call. Two
// types carry them, kIndexedDb to the Session and kIndexedDbCallback to the
// Page, each with an operation first. The library reads what a Page sends as
// hostile input, so each payload is checked here before it acts on it.
//
// Where Mojo hands over a remote, the Page names the other end by a number of
// its own: a connection for each open(), a transaction, and a cursor. A call
// that Mojo answers has a number that its answer names.
//
// docs/runtime.md describes each message.

#ifndef MITTO_INDEXED_DB_MESSAGES_H_
#define MITTO_INDEXED_DB_MESSAGES_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/types/expected.h"
#include "mitto/indexed_db_key.h"

namespace mitto {

// What a Page asks of its Session's IndexedDB.
enum class IndexedDbCallOp : uint8_t {
  // IDBFactory.
  kGetDatabaseInfo = 1,
  kOpen = 2,
  kDeleteDatabase = 3,
  // IDBDatabase, of a connection.
  kCreateTransaction = 4,
  kVersionChangeIgnored = 5,
  kGet = 6,
  kGetAll = 7,
  kOpenCursor = 8,
  kCount = 9,
  kDeleteRange = 10,
  kGetKeyGeneratorCurrentNumber = 11,
  kClear = 12,
  kCreateIndex = 13,
  kDeleteIndex = 14,
  kRenameIndex = 15,
  kRenameObjectStore = 16,
  kAbort = 17,
  kDidBecomeInactive = 18,
  // The Page closed the connection, as dropping its remote does.
  kCloseConnection = 19,
  // IDBTransaction.
  kCreateObjectStore = 20,
  kDeleteObjectStore = 21,
  kPut = 22,
  kSetIndexKeys = 23,
  kSetIndexKeysDone = 24,
  kCommit = 25,
  // IDBCursor.
  kCursorAdvance = 26,
  kCursorContinue = 27,
  kCursorPrefetch = 28,
  kCursorPrefetchReset = 29,
  // The Page let the cursor go, as dropping its remote does.
  kCloseCursor = 30,
  kMaxValue = kCloseCursor,
};

// What the Session tells the Page.
enum class IndexedDbCallbackOp : uint8_t {
  // IDBFactoryClient, of a request.
  kError = 1,
  kBlocked = 2,
  kUpgradeNeeded = 3,
  kOpenSuccess = 4,
  kDeleteSuccess = 5,
  // The answer to kGetDatabaseInfo.
  kDatabaseInfo = 6,
  // IDBDatabaseCallbacks, of a connection.
  kForcedClose = 7,
  kVersionChange = 8,
  kAbort = 9,
  kComplete = 10,
  // The answers to calls.
  kGetResult = 11,
  kGetAllResult = 12,
  kOpenCursorResult = 13,
  kCountResult = 14,
  kSuccessResult = 15,
  kKeyGeneratorResult = 16,
  kPutResult = 17,
  kCursorResult = 18,
  kMaxValue = kCursorResult,
};

// blink.mojom.IDBException's values.
enum class IndexedDbException : uint8_t {
  kNoError = 0,
  kIgnorableAbortError = 11,
  kAbortError = 20,
  kQuotaError = 22,
  kTimeoutError = 23,
  kUnknownError = 28,
  kConstraintError = 29,
  kDataError = 30,
  kVersionError = 33,
};

// blink.mojom.IDBTransactionMode, IDBCursorDirection, IDBPutMode,
// IDBGetAllResultType, IDBTaskType and IDBTransactionDurability.
enum class IndexedDbTransactionMode : uint8_t {
  kReadOnly = 0,
  kReadWrite = 1,
  kVersionChange = 2,
};
enum class IndexedDbCursorDirection : uint8_t {
  kNext = 0,
  kNextNoDuplicate = 1,
  kPrev = 2,
  kPrevNoDuplicate = 3,
};
enum class IndexedDbPutMode : uint8_t {
  kAddOrUpdate = 0,
  kAddOnly = 1,
  kCursorUpdate = 2,
};
enum class IndexedDbGetAllResultType : uint8_t {
  kKeys = 0,
  kValues = 1,
  kRecords = 2,
};
enum class IndexedDbTaskType : uint8_t {
  kNormal = 0,
  kPreemptive = 1,
};
enum class IndexedDbDurability : uint8_t {
  kDefault = 0,
  kStrict = 1,
  kRelaxed = 2,
};

// blink.mojom.IDBIndexMetadata.
struct IndexedDbIndexMetadata {
  IndexedDbIndexMetadata();
  IndexedDbIndexMetadata(const IndexedDbIndexMetadata&);
  IndexedDbIndexMetadata(IndexedDbIndexMetadata&&);
  IndexedDbIndexMetadata& operator=(const IndexedDbIndexMetadata&);
  IndexedDbIndexMetadata& operator=(IndexedDbIndexMetadata&&);
  ~IndexedDbIndexMetadata();

  static constexpr int64_t kInvalidId = -1;

  int64_t id = kInvalidId;
  std::u16string name;
  IDBKeyPath key_path;
  bool unique = false;
  bool multi_entry = false;
};

// blink.mojom.IDBObjectStoreMetadata.
struct IndexedDbObjectStoreMetadata {
  IndexedDbObjectStoreMetadata();
  IndexedDbObjectStoreMetadata(const IndexedDbObjectStoreMetadata&);
  IndexedDbObjectStoreMetadata(IndexedDbObjectStoreMetadata&&);
  IndexedDbObjectStoreMetadata& operator=(const IndexedDbObjectStoreMetadata&);
  IndexedDbObjectStoreMetadata& operator=(IndexedDbObjectStoreMetadata&&);
  ~IndexedDbObjectStoreMetadata();

  static constexpr int64_t kInvalidId = -1;

  int64_t id = kInvalidId;
  std::u16string name;
  IDBKeyPath key_path;
  bool auto_increment = false;
  int64_t max_index_id = 0;
  std::map<int64_t, IndexedDbIndexMetadata> indexes;
};

// blink.mojom.IDBDatabaseMetadata.
struct IndexedDbDatabaseMetadata {
  IndexedDbDatabaseMetadata();
  IndexedDbDatabaseMetadata(const IndexedDbDatabaseMetadata&);
  IndexedDbDatabaseMetadata(IndexedDbDatabaseMetadata&&);
  IndexedDbDatabaseMetadata& operator=(const IndexedDbDatabaseMetadata&);
  IndexedDbDatabaseMetadata& operator=(IndexedDbDatabaseMetadata&&);
  ~IndexedDbDatabaseMetadata();

  // IndexedDBDatabaseMetadata's: no version yet, and the version that a new
  // database has before its first upgrade.
  static constexpr int64_t kNoVersion = -1;
  static constexpr int64_t kDefaultVersion = 0;

  std::u16string name;
  int64_t version = kNoVersion;
  int64_t max_object_store_id = 0;
  std::map<int64_t, IndexedDbObjectStoreMetadata> object_stores;
};

// blink.mojom.IDBIndexKeys.
struct IndexedDbIndexKeys {
  IndexedDbIndexKeys();
  IndexedDbIndexKeys(const IndexedDbIndexKeys&);
  IndexedDbIndexKeys(IndexedDbIndexKeys&&);
  IndexedDbIndexKeys& operator=(const IndexedDbIndexKeys&);
  IndexedDbIndexKeys& operator=(IndexedDbIndexKeys&&);
  ~IndexedDbIndexKeys();

  int64_t index_id = 0;
  std::vector<IDBKey> keys;
};

// blink.mojom.IDBReturnValue: a value, and the primary key that a key
// generator gave it and its key path, which the Page puts into the value.
struct IndexedDbReturnValue {
  IndexedDbReturnValue();
  IndexedDbReturnValue(const IndexedDbReturnValue&);
  IndexedDbReturnValue(IndexedDbReturnValue&&);
  IndexedDbReturnValue& operator=(const IndexedDbReturnValue&);
  IndexedDbReturnValue& operator=(IndexedDbReturnValue&&);
  ~IndexedDbReturnValue();

  // The value as SerializedScriptValue wrote it.
  std::string bits;
  // None, unless a key generator gave the primary key.
  IDBKey primary_key;
  IDBKeyPath key_path;
};

// blink.mojom.IDBRecord.
struct IndexedDbRecord {
  IndexedDbRecord();
  IndexedDbRecord(const IndexedDbRecord&);
  IndexedDbRecord(IndexedDbRecord&&);
  IndexedDbRecord& operator=(const IndexedDbRecord&);
  IndexedDbRecord& operator=(IndexedDbRecord&&);
  ~IndexedDbRecord();

  std::optional<IDBKey> primary_key;
  std::optional<IndexedDbReturnValue> value;
  std::optional<IDBKey> index_key;
};

struct PageIndexedDbCall {
  PageIndexedDbCall();
  PageIndexedDbCall(PageIndexedDbCall&&);
  PageIndexedDbCall& operator=(PageIndexedDbCall&&);
  ~PageIndexedDbCall();

  IndexedDbCallOp op = IndexedDbCallOp::kGetDatabaseInfo;
  // A factory request's number, or the number of a call that is answered.
  uint64_t call = 0;
  uint64_t connection = 0;
  uint64_t transaction = 0;
  uint64_t cursor = 0;
  std::u16string name;
  int64_t version = 0;
  int64_t object_store_id = 0;
  int64_t index_id = IndexedDbIndexMetadata::kInvalidId;
  std::vector<int64_t> scope;
  IndexedDbTransactionMode mode = IndexedDbTransactionMode::kReadOnly;
  IndexedDbDurability durability = IndexedDbDurability::kDefault;
  IndexedDbKeyRange key_range;
  bool key_only = false;
  IndexedDbGetAllResultType result_type = IndexedDbGetAllResultType::kValues;
  uint32_t count = 0;
  IndexedDbCursorDirection direction = IndexedDbCursorDirection::kNext;
  IndexedDbTaskType task_type = IndexedDbTaskType::kNormal;
  IndexedDbIndexMetadata index;
  IDBKeyPath key_path;
  bool auto_increment = false;
  std::string value;
  IDBKey key;
  IDBKey primary_key;
  IndexedDbPutMode put_mode = IndexedDbPutMode::kAddOrUpdate;
  std::vector<IndexedDbIndexKeys> index_keys;
  int64_t num_errors_handled = 0;
};

struct PageIndexedDbCallback {
  PageIndexedDbCallback();
  PageIndexedDbCallback(PageIndexedDbCallback&&);
  PageIndexedDbCallback& operator=(PageIndexedDbCallback&&);
  ~PageIndexedDbCallback();

  IndexedDbCallbackOp op = IndexedDbCallbackOp::kError;
  // The request or call it answers.
  uint64_t call = 0;
  uint64_t connection = 0;
  uint64_t transaction = 0;
  IndexedDbException error = IndexedDbException::kNoError;
  std::u16string message;
  int64_t old_version = 0;
  int64_t new_version = 0;
  bool data_loss = false;
  IndexedDbDatabaseMetadata metadata;
  // kDatabaseInfo's names and versions.
  std::vector<std::pair<std::u16string, int64_t>> databases;
  // A get's, a cursor's or a put's: whether there was nothing, and the key,
  // primary key and value found.
  bool empty = false;
  IDBKey key;
  IDBKey primary_key;
  std::optional<IndexedDbReturnValue> value;
  // kGetAllResult's records, and whether they are the last.
  std::vector<IndexedDbRecord> records;
  bool done = false;
  // kCursorResult's prefetched entries.
  std::vector<IDBKey> keys;
  std::vector<IDBKey> primary_keys;
  std::vector<IndexedDbReturnValue> values;
  // kCountResult's and kSuccessResult's.
  bool success = false;
  uint32_t count = 0;
  // kKeyGeneratorResult's.
  int64_t number = 0;
};

bool IndexedDbCallFits(const PageIndexedDbCall& call);
std::string EncodeIndexedDbCall(const PageIndexedDbCall& call);
bool IndexedDbCallbackFits(const PageIndexedDbCallback& callback);
std::string EncodeIndexedDbCallback(const PageIndexedDbCallback& callback);

base::expected<PageIndexedDbCall, std::string> ReadIndexedDbCall(
    std::string_view payload);
base::expected<PageIndexedDbCallback, std::string> ReadIndexedDbCallback(
    std::string_view payload);

}  // namespace mitto

#endif  // MITTO_INDEXED_DB_MESSAGES_H_
