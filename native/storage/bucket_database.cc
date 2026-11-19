#include "mitto/storage/bucket_database.h"

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

// Chrome's columns that a page can reach, with their names and types. An ID
// goes up from the largest that any row has had, as AUTOINCREMENT keeps it,
// so that no bucket ever has the ID of one that went.
bool CreateSchema(sql::Database& database) {
  return database.Execute(
             "CREATE TABLE buckets("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "storage_key TEXT NOT NULL,"
             "name TEXT NOT NULL,"
             "expiration INTEGER NOT NULL,"
             "quota INTEGER NOT NULL,"
             "persistent INTEGER NOT NULL,"
             "durability INTEGER NOT NULL"
             ") STRICT") &&
         database.Execute(
             "CREATE UNIQUE INDEX buckets_by_storage_key "
             "ON buckets(storage_key, name)");
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

BucketInfo::BucketInfo() = default;
BucketInfo::BucketInfo(const BucketInfo&) = default;
BucketInfo::BucketInfo(BucketInfo&&) = default;
BucketInfo& BucketInfo::operator=(const BucketInfo&) = default;
BucketInfo& BucketInfo::operator=(BucketInfo&&) = default;
BucketInfo::~BucketInfo() = default;

BucketDatabase::Contents::Contents() = default;
BucketDatabase::Contents::Contents(Contents&&) = default;
BucketDatabase::Contents& BucketDatabase::Contents::operator=(Contents&&) =
    default;
BucketDatabase::Contents::~Contents() = default;

BucketDatabase::BucketDatabase() = default;
BucketDatabase::~BucketDatabase() = default;

bool BucketDatabase::Open(const base::FilePath& path) {
  CHECK(!database_);
  const bool in_memory = path.empty();
  database_ = std::make_unique<sql::Database>(
      sql::DatabaseOptions().set_wal_mode(!in_memory).set_mmap_enabled(false),
      sql::Database::Tag("Quota"));
  if (!(in_memory ? database_->OpenInMemory() : database_->Open(path)) ||
      !InitializeSchema(*database_)) {
    database_.reset();
    return false;
  }
  return true;
}

std::optional<BucketDatabase::Contents> BucketDatabase::ReadAll() {
  if (!database_) {
    return std::nullopt;
  }
  Contents contents;
  sql::Statement statement(database_->GetUniqueStatement(
      "SELECT id, storage_key, name, expiration, quota, persistent, durability "
      "FROM buckets ORDER BY id"));
  while (statement.Step()) {
    BucketInfo& bucket = contents.buckets.emplace_back();
    bucket.id = statement.ColumnInt64(0);
    bucket.storage_key = statement.ColumnString(1);
    bucket.name = statement.ColumnString(2);
    bucket.expiration = statement.ColumnTime(3);
    bucket.quota = statement.ColumnInt64(4);
    bucket.persistent = statement.ColumnBool(5);
    bucket.durability = statement.ColumnInt(6) == 1
                            ? BucketDurability::kStrict
                            : BucketDurability::kRelaxed;
  }
  if (!statement.Succeeded()) {
    return std::nullopt;
  }
  sql::Statement sequence(database_->GetUniqueStatement(
      "SELECT seq FROM sqlite_sequence WHERE name = 'buckets'"));
  if (sequence.Step()) {
    contents.last_id = sequence.ColumnInt64(0);
  } else if (!sequence.Succeeded()) {
    return std::nullopt;
  }
  return contents;
}

bool BucketDatabase::Insert(const BucketInfo& bucket) {
  if (!database_) {
    return false;
  }
  sql::Statement statement(database_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT INTO buckets(id, storage_key, name, expiration, quota, "
      "persistent, durability) VALUES(?, ?, ?, ?, ?, ?, ?)"));
  statement.BindInt64(0, bucket.id);
  statement.BindString(1, bucket.storage_key);
  statement.BindString(2, bucket.name);
  statement.BindTime(3, bucket.expiration);
  statement.BindInt64(4, bucket.quota);
  statement.BindBool(5, bucket.persistent);
  statement.BindInt(6, static_cast<int>(bucket.durability));
  return statement.Run();
}

bool BucketDatabase::UpdateExpiration(int64_t id, base::Time expiration) {
  if (!database_) {
    return false;
  }
  sql::Statement statement(database_->GetCachedStatement(
      SQL_FROM_HERE, "UPDATE buckets SET expiration = ? WHERE id = ?"));
  statement.BindTime(0, expiration);
  statement.BindInt64(1, id);
  return statement.Run();
}

bool BucketDatabase::Delete(int64_t id) {
  if (!database_) {
    return false;
  }
  sql::Statement statement(database_->GetCachedStatement(
      SQL_FROM_HERE, "DELETE FROM buckets WHERE id = ?"));
  statement.BindInt64(0, id);
  return statement.Run();
}

}  // namespace mitto
