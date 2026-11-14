#include "mitto/storage/local_storage_database.h"

#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "sql/database.h"
#include "sql/meta_table.h"
#include "sql/statement.h"
#include "sql/transaction.h"
#include "third_party/zstd/src/lib/zstd.h"

namespace mitto {

namespace {

constexpr int kCurrentSchemaVersion = 1;
constexpr int kCompatibleSchemaVersion = kCurrentSchemaVersion;

// How a value is kept, which the file stores: Chrome's CompressionType. mitto
// writes zstd, as Chrome on desktop does, and reads what it writes.
constexpr int kUncompressed = 0;
constexpr int kZstd = 1;

// A value shorter than this is kept as it is.
constexpr size_t kMinimumCompressionSize = 350;
// A compressed value is kept only if it is at most this share of the value.
constexpr float kMinimumCompressionRatio = 0.8f;
// A level that compresses about as well as Snappy, which Chrome picks.
constexpr int kCompressionLevel = -4;

bool CreateSchema(sql::Database& database) {
  return database.Execute(
             "CREATE TABLE maps("
             "row_id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "storage_key BLOB NOT NULL,"
             "last_accessed INTEGER,"
             "last_modified INTEGER,"
             "total_size INTEGER"
             ")") &&
         database.Execute(
             "CREATE UNIQUE INDEX maps_by_storage_key ON maps(storage_key)") &&
         database.Execute(
             "CREATE TABLE map_entries("
             "map_id INTEGER NOT NULL,"
             "value_compression_type INTEGER NOT NULL,"
             "key BLOB NOT NULL,"
             "value BLOB NOT NULL,"
             "PRIMARY KEY(map_id, key)"
             ") WITHOUT ROWID;");
}

// Returns the type and the bytes to keep of `value`.
std::pair<int, std::string> Compress(std::string value) {
  if (value.size() < kMinimumCompressionSize) {
    return {kUncompressed, std::move(value)};
  }
  std::string compressed(ZSTD_compressBound(value.size()), '\0');
  const size_t size =
      ZSTD_compress(compressed.data(), compressed.size(), value.data(),
                    value.size(), kCompressionLevel);
  if (ZSTD_isError(size) ||
      static_cast<float>(size) >
          static_cast<float>(value.size()) * kMinimumCompressionRatio) {
    return {kUncompressed, std::move(value)};
  }
  compressed.resize(size);
  return {kZstd, std::move(compressed)};
}

std::optional<std::string> Decompress(int type, std::string data) {
  if (type == kUncompressed) {
    return data;
  }
  if (type != kZstd) {
    return std::nullopt;
  }
  const uint64_t size = ZSTD_getFrameContentSize(data.data(), data.size());
  if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR ||
      !base::IsValueInRangeForNumericType<size_t>(size)) {
    return std::nullopt;
  }
  std::string value(static_cast<size_t>(size), '\0');
  if (ZSTD_isError(ZSTD_decompress(value.data(), value.size(), data.data(),
                                   data.size()))) {
    return std::nullopt;
  }
  return value;
}

void BindOptionalTime(sql::Statement& statement,
                      int index,
                      std::optional<base::Time> time) {
  if (time) {
    statement.BindTime(index, *time);
  } else {
    statement.BindNull(index);
  }
}

void BindOptionalSize(sql::Statement& statement,
                      int index,
                      std::optional<uint64_t> size) {
  if (size) {
    statement.BindInt64(index, base::checked_cast<int64_t>(*size));
  } else {
    statement.BindNull(index);
  }
}

}  // namespace

LocalStorageDatabase::MapUpdate::MapUpdate(StorageKey storage_key)
    : storage_key(std::move(storage_key)) {}
LocalStorageDatabase::MapUpdate::MapUpdate(MapUpdate&&) = default;
LocalStorageDatabase::MapUpdate& LocalStorageDatabase::MapUpdate::operator=(
    MapUpdate&&) = default;
LocalStorageDatabase::MapUpdate::~MapUpdate() = default;

LocalStorageDatabase::LocalStorageDatabase() = default;
LocalStorageDatabase::~LocalStorageDatabase() = default;

bool LocalStorageDatabase::Open(const base::FilePath& path) {
  CHECK(!database_);
  const bool in_memory = path.empty();
  database_ = std::make_unique<sql::Database>(
      sql::DatabaseOptions().set_wal_mode(true).set_mmap_enabled(false),
      in_memory ? sql::Database::Tag("LocalStorageEphemeral")
                : sql::Database::Tag("LocalStorage"));
  if (!(in_memory ? database_->OpenInMemory() : database_->Open(path))) {
    database_.reset();
    return false;
  }
  sql::Transaction transaction(database_.get());
  if (!transaction.Begin()) {
    database_.reset();
    return false;
  }
  if (!sql::MetaTable::DoesTableExist(database_.get()) &&
      !CreateSchema(*database_)) {
    database_.reset();
    return false;
  }
  sql::MetaTable meta_table;
  if (!meta_table.Init(database_.get(), kCurrentSchemaVersion,
                       kCompatibleSchemaVersion) ||
      meta_table.GetCompatibleVersionNumber() > kCurrentSchemaVersion ||
      !transaction.Commit()) {
    database_.reset();
    return false;
  }
  return true;
}

std::optional<StorageValueMap> LocalStorageDatabase::ReadMap(
    const StorageKey& storage_key) {
  if (!database_) {
    return std::nullopt;
  }
  sql::Transaction transaction(database_.get());
  if (!transaction.Begin()) {
    return std::nullopt;
  }
  std::optional<std::optional<int64_t>> map_id = FindMapId(storage_key);
  if (!map_id) {
    return std::nullopt;
  }
  StorageValueMap map;
  if (!*map_id) {
    return map;
  }
  sql::Statement statement(
      database_->GetCachedStatement(SQL_FROM_HERE,
                                    "SELECT key, value_compression_type, value "
                                    "FROM map_entries WHERE map_id = ?"));
  statement.BindInt64(0, **map_id);
  while (statement.Step()) {
    std::optional<std::string> value =
        Decompress(statement.ColumnInt(1), statement.ColumnBlobAsString(2));
    if (!value) {
      return std::nullopt;
    }
    map.emplace(statement.ColumnBlobAsString(0), *std::move(value));
  }
  if (!statement.Succeeded() || !transaction.Commit()) {
    return std::nullopt;
  }
  return map;
}

bool LocalStorageDatabase::UpdateMaps(std::vector<MapUpdate> updates) {
  if (!database_) {
    return false;
  }
  sql::Transaction transaction(database_.get());
  if (!transaction.Begin()) {
    return false;
  }
  for (MapUpdate& update : updates) {
    std::optional<std::optional<int64_t>> found = FindMapId(update.storage_key);
    if (!found) {
      return false;
    }
    int64_t map_id = 0;
    if (update.delete_usage) {
      // An empty area adds nothing, and one that was never written is not
      // there to delete.
      CHECK(update.entries_to_add.empty());
      if (!*found) {
        continue;
      }
      map_id = **found;
      sql::Statement statement(database_->GetCachedStatement(
          SQL_FROM_HERE, "DELETE FROM maps WHERE storage_key = ?"));
      statement.BindBlob(0, update.storage_key.Serialize());
      if (!statement.Run()) {
        return false;
      }
    } else {
      if (!PutMapMetadata(update.storage_key, update.last_accessed,
                          update.last_modified, update.total_size)) {
        return false;
      }
      found = FindMapId(update.storage_key);
      if (!found || !*found) {
        return false;
      }
      map_id = **found;
    }
    if (update.clear_all_first) {
      sql::Statement statement(database_->GetCachedStatement(
          SQL_FROM_HERE, "DELETE FROM map_entries WHERE map_id = ?"));
      statement.BindInt64(0, map_id);
      if (!statement.Run()) {
        return false;
      }
    } else {
      sql::Statement statement(database_->GetCachedStatement(
          SQL_FROM_HERE,
          "DELETE FROM map_entries WHERE map_id = ? AND key = ?"));
      statement.BindInt64(0, map_id);
      for (std::string& key : update.keys_to_delete) {
        statement.BindBlob(1, std::move(key));
        if (!statement.Run()) {
          return false;
        }
        statement.Reset(/*clear_bound_vars=*/false);
      }
    }
    sql::Statement statement(database_->GetCachedStatement(
        SQL_FROM_HERE,
        "INSERT OR REPLACE INTO map_entries("
        "map_id, key, value_compression_type, value"
        ") VALUES(?, ?, ?, ?)"));
    statement.BindInt64(0, map_id);
    for (auto& [key, value] : update.entries_to_add) {
      auto [type, data] = Compress(std::move(value));
      statement.BindBlob(1, std::move(key));
      statement.BindInt(2, type);
      statement.BindBlob(3, std::move(data));
      if (!statement.Run()) {
        return false;
      }
      statement.Reset(/*clear_bound_vars=*/false);
    }
  }
  return transaction.Commit();
}

bool LocalStorageDatabase::PutLastAccessed(
    const std::vector<StorageKey>& storage_keys,
    base::Time last_accessed) {
  if (!database_) {
    return false;
  }
  sql::Transaction transaction(database_.get());
  if (!transaction.Begin()) {
    return false;
  }
  for (const StorageKey& key : storage_keys) {
    if (!PutMapMetadata(key, last_accessed, std::nullopt, std::nullopt)) {
      return false;
    }
  }
  return transaction.Commit();
}

std::optional<std::optional<int64_t>> LocalStorageDatabase::FindMapId(
    const StorageKey& key) {
  sql::Statement statement(database_->GetCachedStatement(
      SQL_FROM_HERE, "SELECT row_id FROM maps WHERE storage_key = ?"));
  statement.BindBlob(0, key.Serialize());
  if (!statement.Step()) {
    if (!statement.Succeeded()) {
      return std::nullopt;
    }
    return std::optional<int64_t>();
  }
  return std::optional<int64_t>(statement.ColumnInt64(0));
}

bool LocalStorageDatabase::PutMapMetadata(
    const StorageKey& key,
    std::optional<base::Time> last_accessed,
    std::optional<base::Time> last_modified,
    std::optional<uint64_t> total_size) {
  CHECK(database_->HasActiveTransactions());
  const std::string serialized = key.Serialize();
  // An update keeps what it is not given, and a new area gets a row.
  sql::Statement update(database_->GetCachedStatement(
      SQL_FROM_HERE,
      "UPDATE maps "
      "SET "
      "last_accessed = COALESCE(?, last_accessed),"
      "last_modified = COALESCE(?, last_modified),"
      "total_size = COALESCE(?, total_size) "
      "WHERE storage_key = ?"));
  BindOptionalTime(update, 0, last_accessed);
  BindOptionalTime(update, 1, last_modified);
  BindOptionalSize(update, 2, total_size);
  update.BindBlob(3, serialized);
  if (!update.Run()) {
    return false;
  }
  if (database_->GetLastChangeCount() != 0) {
    return true;
  }
  sql::Statement insert(database_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT INTO maps"
      "(storage_key, last_accessed, last_modified, total_size) "
      "VALUES(?, ?, ?, ?)"));
  insert.BindBlob(0, serialized);
  BindOptionalTime(insert, 1, last_accessed);
  BindOptionalTime(insert, 2, last_modified);
  BindOptionalSize(insert, 3, total_size);
  return insert.Run();
}

}  // namespace mitto
