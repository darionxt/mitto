#include "mitto/indexed_db/indexed_db_file.h"

#include <utility>

#include "base/check.h"
#include "sql/database.h"
#include "sql/meta_table.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace mitto {

namespace {

constexpr int kCurrentSchemaVersion = 1;
constexpr int kCompatibleSchemaVersion = kCurrentSchemaVersion;

// A table with row IDs, rather than one keyed by its primary key, since its
// values are as large as a page's records, which such a table keeps apart from
// the index of its keys.
bool CreateSchema(sql::Database& database) {
  return database.Execute(
             "CREATE TABLE entries("
             "storage_key BLOB NOT NULL,"
             "key BLOB NOT NULL,"
             "value BLOB NOT NULL"
             ")") &&
         database.Execute(
             "CREATE UNIQUE INDEX entries_by_key ON entries(storage_key, key)");
}

// Makes the schema of a new database, or checks that of one that a version
// this one can read wrote. The transaction and the meta table end with this,
// so before the caller lets a database go that this refused.
bool InitializeSchema(sql::Database& database) {
  sql::Transaction transaction(&database);
  if (!transaction.Begin()) {
    return false;
  }
  if (!sql::MetaTable::DoesTableExist(&database) && !CreateSchema(database)) {
    return false;
  }
  sql::MetaTable meta_table;
  return meta_table.Init(&database, kCurrentSchemaVersion,
                         kCompatibleSchemaVersion) &&
         meta_table.GetCompatibleVersionNumber() <= kCurrentSchemaVersion &&
         transaction.Commit();
}

}  // namespace

IndexedDbFile::IndexedDbFile() = default;
IndexedDbFile::~IndexedDbFile() = default;

bool IndexedDbFile::Open(const base::FilePath& path) {
  CHECK(!database_);
  database_ = std::make_unique<sql::Database>(
      sql::DatabaseOptions().set_wal_mode(true).set_mmap_enabled(false),
      sql::Database::Tag("IndexedDB"));
  if (!database_->Open(path) || !InitializeSchema(*database_)) {
    database_.reset();
    return false;
  }
  return true;
}

std::optional<KvEntries> IndexedDbFile::Read(const std::string& storage_key) {
  if (!database_) {
    return std::nullopt;
  }
  sql::Statement statement(database_->GetCachedStatement(
      SQL_FROM_HERE, "SELECT key, value FROM entries WHERE storage_key = ?"));
  statement.BindBlob(0, storage_key);
  KvEntries entries;
  while (statement.Step()) {
    entries.emplace(statement.ColumnBlobAsString(0),
                    statement.ColumnBlobAsString(1));
  }
  if (!statement.Succeeded()) {
    return std::nullopt;
  }
  return entries;
}

std::optional<int64_t> IndexedDbFile::Usage(const std::string& storage_key) {
  if (!database_) {
    return std::nullopt;
  }
  sql::Statement statement(database_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT SUM(LENGTH(key) + LENGTH(value)) FROM entries "
      "WHERE storage_key = ?"));
  statement.BindBlob(0, storage_key);
  if (!statement.Step()) {
    return std::nullopt;
  }
  return statement.ColumnInt64(0);
}

bool IndexedDbFile::Write(const std::string& storage_key,
                          const KvBatch& batch) {
  if (!database_) {
    return false;
  }
  sql::Transaction transaction(database_.get());
  if (!transaction.Begin()) {
    return false;
  }
  for (const auto& [key, value] : batch) {
    if (value) {
      sql::Statement insert(database_->GetCachedStatement(
          SQL_FROM_HERE,
          "INSERT OR REPLACE INTO entries(storage_key, key, value) "
          "VALUES(?, ?, ?)"));
      insert.BindBlob(0, storage_key);
      insert.BindBlob(1, key);
      insert.BindBlob(2, *value);
      if (!insert.Run()) {
        return false;
      }
    } else {
      sql::Statement remove(database_->GetCachedStatement(
          SQL_FROM_HERE,
          "DELETE FROM entries WHERE storage_key = ? AND key = ?"));
      remove.BindBlob(0, storage_key);
      remove.BindBlob(1, key);
      if (!remove.Run()) {
        return false;
      }
    }
  }
  return transaction.Commit();
}

bool IndexedDbFile::Delete(const std::string& storage_key) {
  if (!database_) {
    return false;
  }
  sql::Statement statement(database_->GetCachedStatement(
      SQL_FROM_HERE, "DELETE FROM entries WHERE storage_key = ?"));
  statement.BindBlob(0, storage_key);
  return statement.Run();
}

}  // namespace mitto
