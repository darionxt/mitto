// The buckets of a Session's storage keys, as Chrome's QuotaDatabase keeps a
// profile's in the table `buckets` of its file QuotaManager: a row for each
// bucket, with the ID that the Session gave it, its storage key, its name and
// the policies that the page asked for. A Session with a storage directory
// keeps them in the file QuotaManager there, and one without in memory, as
// Incognito does. It blocks, so it lives on a sequence that may.
//
// Upstream: storage/browser/quota/quota_database.cc

#ifndef MITTO_STORAGE_BUCKET_DATABASE_H_
#define MITTO_STORAGE_BUCKET_DATABASE_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"

namespace sql {
class Database;
}  // namespace sql

namespace mitto {

// The file's name in a Session's storage directory, as Chrome names it in a
// profile's WebStorage.
inline constexpr base::FilePath::CharType kBucketDatabaseFileName[] =
    FILE_PATH_LITERAL("QuotaManager");

// The name of a storage key's default bucket, which no page can give a bucket
// of its own, since a bucket's name never starts with "_".
inline constexpr char kDefaultBucketName[] = "_default";

// blink.mojom.BucketDurability.
enum class BucketDurability : uint8_t {
  kRelaxed = 0,
  kStrict = 1,
};

// storage::BucketInfo, of what a page can reach.
struct BucketInfo {
  BucketInfo();
  BucketInfo(const BucketInfo&);
  BucketInfo(BucketInfo&&);
  BucketInfo& operator=(const BucketInfo&);
  BucketInfo& operator=(BucketInfo&&);
  ~BucketInfo();

  bool is_default() const { return name == kDefaultBucketName; }

  // Larger than that of each bucket before it, in this Session's storage and
  // any before it of its directory.
  int64_t id = 0;
  // StorageKey::Serialize()'s.
  std::string storage_key;
  std::string name;
  // Null for a bucket that does not expire.
  base::Time expiration;
  // 0 for a bucket whose quota is its storage key's.
  int64_t quota = 0;
  bool persistent = false;
  BucketDurability durability = BucketDurability::kRelaxed;
};

class BucketDatabase {
 public:
  BucketDatabase();
  BucketDatabase(const BucketDatabase&) = delete;
  BucketDatabase& operator=(const BucketDatabase&) = delete;
  ~BucketDatabase();

  // Opens the database at `path`, an absolute path, and makes its tables if it
  // is new, or one in memory for an empty path. Returns false if it cannot,
  // or if a later version wrote it.
  bool Open(const base::FilePath& path);

  // Every bucket, and the largest ID that any bucket has ever had, or nullopt
  // if they cannot be read.
  struct Contents {
    Contents();
    Contents(Contents&&);
    Contents& operator=(Contents&&);
    ~Contents();

    std::vector<BucketInfo> buckets;
    int64_t last_id = 0;
  };
  std::optional<Contents> ReadAll();

  // Each returns whether it could.
  bool Insert(const BucketInfo& bucket);
  bool UpdateExpiration(int64_t id, base::Time expiration);
  bool Delete(int64_t id);

 private:
  std::unique_ptr<sql::Database> database_;
};

}  // namespace mitto

#endif  // MITTO_STORAGE_BUCKET_DATABASE_H_
