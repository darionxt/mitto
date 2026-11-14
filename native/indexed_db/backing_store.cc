#include "mitto/indexed_db/backing_store.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/pickle.h"

namespace mitto {
namespace {

// The spaces of keys.
constexpr char kGlobalSpace = 0x00;
constexpr char kDatabaseSpace = 0x01;
// Within the global space.
constexpr char kMaxDatabaseIdType = 0x00;
constexpr char kDatabaseNameType = 0x01;
// Within a database's space.
constexpr char kDatabaseMetadataType = 0x00;
constexpr char kObjectStoreMetadataType = 0x01;
constexpr char kIndexMetadataType = 0x02;
constexpr char kRecordType = 0x10;
constexpr char kIndexEntryType = 0x11;
constexpr char kBackReferenceType = 0x12;

// What comes after every encoding that a key starts with: greater than any
// type byte of the sortable encoding, so a bound of a key and this lies past
// every entry of that key.
constexpr char kPastKey = 0x60;

// Changes with any change to the layout or to what the values hold.
constexpr int kSchemaVersion = 1;

std::string BigEndian(int64_t value) {
  std::string bytes(8, '\0');
  const uint64_t bits = static_cast<uint64_t>(value);
  for (size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<char>((bits >> (56 - 8 * i)) & 0xff);
  }
  return bytes;
}

std::string DatabasePrefix(int64_t database_id) {
  return std::string(1, kDatabaseSpace) + BigEndian(database_id);
}

std::string DatabaseMetadataKey(int64_t database_id) {
  return DatabasePrefix(database_id) + kDatabaseMetadataType;
}

std::string ObjectStoreMetadataPrefix(int64_t database_id) {
  return DatabasePrefix(database_id) + kObjectStoreMetadataType;
}

std::string ObjectStoreMetadataKey(int64_t database_id, int64_t store_id) {
  return ObjectStoreMetadataPrefix(database_id) + BigEndian(store_id);
}

std::string IndexMetadataPrefix(int64_t database_id, int64_t store_id) {
  return DatabasePrefix(database_id) + kIndexMetadataType + BigEndian(store_id);
}

std::string IndexMetadataKey(int64_t database_id,
                             int64_t store_id,
                             int64_t index_id) {
  return IndexMetadataPrefix(database_id, store_id) + BigEndian(index_id);
}

std::string RecordPrefix(int64_t database_id, int64_t store_id) {
  return DatabasePrefix(database_id) + kRecordType + BigEndian(store_id);
}

std::string IndexEntriesOfStorePrefix(int64_t database_id, int64_t store_id) {
  return DatabasePrefix(database_id) + kIndexEntryType + BigEndian(store_id);
}

std::string IndexEntryPrefix(int64_t database_id,
                             int64_t store_id,
                             int64_t index_id) {
  return IndexEntriesOfStorePrefix(database_id, store_id) + BigEndian(index_id);
}

std::string BackReferencePrefix(int64_t database_id, int64_t store_id) {
  return DatabasePrefix(database_id) + kBackReferenceType + BigEndian(store_id);
}

std::string NameKey(const std::u16string& name) {
  return std::string{kGlobalSpace, kDatabaseNameType} +
         EncodeSortableIDBKey(IDBKey::String(name));
}

std::string MaxDatabaseIdKey() {
  return std::string{kGlobalSpace, kMaxDatabaseIdType};
}

// The least key greater than every key that starts with `prefix`.
std::string PrefixEnd(std::string prefix) {
  while (!prefix.empty() && static_cast<unsigned char>(prefix.back()) == 0xff) {
    prefix.pop_back();
  }
  CHECK(!prefix.empty());
  prefix.back() =
      static_cast<char>(static_cast<unsigned char>(prefix.back()) + 1);
  return prefix;
}

// The bounds in the store of `range`, under `prefix`.
std::pair<std::string, std::string> Bounds(const std::string& prefix,
                                           const IndexedDbKeyRange& range) {
  std::string lower = prefix;
  if (range.lower.IsValid()) {
    lower += EncodeSortableIDBKey(range.lower);
    if (range.lower_open) {
      lower += kPastKey;
    }
  }
  std::string upper;
  if (range.upper.IsValid()) {
    upper = prefix + EncodeSortableIDBKey(range.upper);
    if (!range.upper_open) {
      upper += kPastKey;
    }
  } else {
    upper = PrefixEnd(prefix);
  }
  return {std::move(lower), std::move(upper)};
}

std::string Int64Value(int64_t value) {
  base::Pickle pickle;
  pickle.WriteInt64(value);
  return std::string(pickle.AsStringView());
}

std::optional<int64_t> ReadInt64Value(const std::optional<std::string>& value) {
  if (!value) {
    return std::nullopt;
  }
  base::PickleIterator iterator =
      base::PickleIterator::WithData(base::as_byte_span(*value));
  int64_t number = 0;
  if (!iterator.ReadInt64(&number)) {
    return std::nullopt;
  }
  return number;
}

void WriteKeyPath(base::Pickle& pickle, const IDBKeyPath& key_path) {
  pickle.WriteInt(static_cast<int>(key_path.type));
  pickle.WriteString16(key_path.string);
  pickle.WriteUInt64(key_path.array.size());
  for (const std::u16string& path : key_path.array) {
    pickle.WriteString16(path);
  }
}

bool ReadKeyPath(base::PickleIterator& iterator, IDBKeyPath& key_path) {
  int type = 0;
  uint64_t count = 0;
  if (!iterator.ReadInt(&type) || type < 0 ||
      type > static_cast<int>(IDBKeyPathType::kArray) ||
      !iterator.ReadString16(&key_path.string) ||
      !iterator.ReadUInt64(&count)) {
    return false;
  }
  key_path.type = static_cast<IDBKeyPathType>(type);
  key_path.array.clear();
  for (uint64_t i = 0; i < count; ++i) {
    std::u16string path;
    if (!iterator.ReadString16(&path)) {
      return false;
    }
    key_path.array.push_back(std::move(path));
  }
  return true;
}

std::string DatabaseMetadataValue(const std::u16string& name,
                                  int64_t version,
                                  int64_t max_object_store_id) {
  base::Pickle pickle;
  pickle.WriteInt(kSchemaVersion);
  pickle.WriteString16(name);
  pickle.WriteInt64(version);
  pickle.WriteInt64(max_object_store_id);
  return std::string(pickle.AsStringView());
}

std::string ObjectStoreMetadataValue(const IndexedDbObjectStoreMetadata& store,
                                     int64_t key_generator_current_number) {
  base::Pickle pickle;
  pickle.WriteString16(store.name);
  WriteKeyPath(pickle, store.key_path);
  pickle.WriteBool(store.auto_increment);
  pickle.WriteInt64(store.max_index_id);
  pickle.WriteInt64(key_generator_current_number);
  return std::string(pickle.AsStringView());
}

std::string IndexMetadataValue(const IndexedDbIndexMetadata& index) {
  base::Pickle pickle;
  pickle.WriteString16(index.name);
  WriteKeyPath(pickle, index.key_path);
  pickle.WriteBool(index.unique);
  pickle.WriteBool(index.multi_entry);
  return std::string(pickle.AsStringView());
}

// A record's index entries, which its back reference lists, so that a put or
// a delete of the record finds them.
using IndexEntries = std::vector<std::pair<int64_t, std::string>>;

std::string BackReferenceValue(const IndexEntries& entries) {
  base::Pickle pickle;
  pickle.WriteUInt64(entries.size());
  for (const auto& [index_id, key] : entries) {
    pickle.WriteInt64(index_id);
    pickle.WriteString(key);
  }
  return std::string(pickle.AsStringView());
}

IndexEntries ReadBackReference(const std::optional<std::string>& value) {
  IndexEntries entries;
  if (!value) {
    return entries;
  }
  base::PickleIterator iterator =
      base::PickleIterator::WithData(base::as_byte_span(*value));
  uint64_t count = 0;
  if (!iterator.ReadUInt64(&count)) {
    return entries;
  }
  for (uint64_t i = 0; i < count; ++i) {
    int64_t index_id = 0;
    std::string key;
    if (!iterator.ReadInt64(&index_id) || !iterator.ReadString(&key)) {
      break;
    }
    entries.emplace_back(index_id, std::move(key));
  }
  return entries;
}

}  // namespace

IndexedDbBackingStore::IndexedDbBackingStore(std::unique_ptr<KvStore> kv)
    : kv_(std::move(kv)) {}

IndexedDbBackingStore::~IndexedDbBackingStore() = default;

std::vector<std::pair<std::u16string, int64_t>>
IndexedDbBackingStore::GetDatabaseNamesAndVersions() {
  std::vector<std::pair<std::u16string, int64_t>> databases;
  const std::string prefix{kGlobalSpace, kDatabaseNameType};
  const std::string end = PrefixEnd(prefix);
  std::string from = prefix;
  while (std::optional<KvStore::Entry> entry =
             kv_->Seek(from, prefix, end, /*reverse=*/false)) {
    from = entry->key + '\0';
    const IDBKey name = DecodeSortableIDBKey(
        std::string_view(entry->key).substr(prefix.size()));
    const std::optional<int64_t> database_id = ReadInt64Value(entry->value);
    if (name.type() != IDBKeyType::kString || !database_id) {
      continue;
    }
    std::optional<std::pair<int64_t, IndexedDbDatabaseMetadata>> database =
        LoadDatabase(name.string());
    // A database that no upgrade ever committed has no version yet, and is
    // not listed, as Chrome lists none.
    if (database &&
        database->second.version != IndexedDbDatabaseMetadata::kNoVersion) {
      databases.emplace_back(name.string(), database->second.version);
    }
  }
  return databases;
}

std::optional<std::pair<int64_t, IndexedDbDatabaseMetadata>>
IndexedDbBackingStore::LoadDatabase(const std::u16string& name) {
  const std::optional<int64_t> database_id =
      ReadInt64Value(kv_->Get(NameKey(name)));
  if (!database_id) {
    return std::nullopt;
  }
  IndexedDbDatabaseMetadata metadata;
  metadata.name = name;
  const std::optional<std::string> value =
      kv_->Get(DatabaseMetadataKey(*database_id));
  if (!value) {
    return std::nullopt;
  }
  {
    base::PickleIterator iterator =
        base::PickleIterator::WithData(base::as_byte_span(*value));
    int schema = 0;
    std::u16string stored_name;
    if (!iterator.ReadInt(&schema) || schema != kSchemaVersion ||
        !iterator.ReadString16(&stored_name) ||
        !iterator.ReadInt64(&metadata.version) ||
        !iterator.ReadInt64(&metadata.max_object_store_id)) {
      return std::nullopt;
    }
  }
  const std::string stores_prefix = ObjectStoreMetadataPrefix(*database_id);
  const std::string stores_end = PrefixEnd(stores_prefix);
  std::string from = stores_prefix;
  while (std::optional<KvStore::Entry> entry =
             kv_->Seek(from, stores_prefix, stores_end, /*reverse=*/false)) {
    from = entry->key + '\0';
    IndexedDbObjectStoreMetadata store;
    base::PickleIterator iterator =
        base::PickleIterator::WithData(base::as_byte_span(entry->value));
    int64_t key_generator = 0;
    if (entry->key.size() != stores_prefix.size() + 8 ||
        !iterator.ReadString16(&store.name) ||
        !ReadKeyPath(iterator, store.key_path) ||
        !iterator.ReadBool(&store.auto_increment) ||
        !iterator.ReadInt64(&store.max_index_id) ||
        !iterator.ReadInt64(&key_generator)) {
      return std::nullopt;
    }
    store.id = 0;
    for (size_t i = stores_prefix.size(); i < entry->key.size(); ++i) {
      store.id = (store.id << 8) | static_cast<unsigned char>(entry->key[i]);
    }
    const std::string indexes_prefix =
        IndexMetadataPrefix(*database_id, store.id);
    const std::string indexes_end = PrefixEnd(indexes_prefix);
    std::string index_from = indexes_prefix;
    while (std::optional<KvStore::Entry> index_entry = kv_->Seek(
               index_from, indexes_prefix, indexes_end, /*reverse=*/false)) {
      index_from = index_entry->key + '\0';
      IndexedDbIndexMetadata index;
      base::PickleIterator index_iterator = base::PickleIterator::WithData(
          base::as_byte_span(index_entry->value));
      if (index_entry->key.size() != indexes_prefix.size() + 8 ||
          !index_iterator.ReadString16(&index.name) ||
          !ReadKeyPath(index_iterator, index.key_path) ||
          !index_iterator.ReadBool(&index.unique) ||
          !index_iterator.ReadBool(&index.multi_entry)) {
        return std::nullopt;
      }
      index.id = 0;
      for (size_t i = indexes_prefix.size(); i < index_entry->key.size(); ++i) {
        index.id =
            (index.id << 8) | static_cast<unsigned char>(index_entry->key[i]);
      }
      const int64_t index_id = index.id;
      store.indexes.emplace(index_id, std::move(index));
    }
    const int64_t store_id = store.id;
    metadata.object_stores.emplace(store_id, std::move(store));
  }
  return std::make_pair(*database_id, std::move(metadata));
}

std::optional<int64_t> IndexedDbBackingStore::CreateDatabase(
    const std::u16string& name) {
  const int64_t database_id =
      ReadInt64Value(kv_->Get(MaxDatabaseIdKey())).value_or(0) + 1;
  KvBatch batch;
  batch[MaxDatabaseIdKey()] = Int64Value(database_id);
  batch[NameKey(name)] = Int64Value(database_id);
  batch[DatabaseMetadataKey(database_id)] = DatabaseMetadataValue(
      name, IndexedDbDatabaseMetadata::kNoVersion, /*max_object_store_id=*/0);
  if (!kv_->Commit(batch)) {
    return std::nullopt;
  }
  return database_id;
}

bool IndexedDbBackingStore::DeleteDatabase(int64_t database_id,
                                           const std::u16string& name) {
  KvBatch batch;
  batch[NameKey(name)] = std::nullopt;
  const std::string prefix = DatabasePrefix(database_id);
  const std::string end = PrefixEnd(prefix);
  std::string from = prefix;
  while (std::optional<KvStore::Entry> entry =
             kv_->Seek(from, prefix, end, /*reverse=*/false)) {
    from = entry->key + '\0';
    batch[entry->key] = std::nullopt;
  }
  return kv_->Commit(batch);
}

IndexedDbBackingStore::Transaction::Transaction(IndexedDbBackingStore& store,
                                                int64_t database_id)
    : store_(store), database_id_(database_id) {}

IndexedDbBackingStore::Transaction::~Transaction() = default;

bool IndexedDbBackingStore::Transaction::Commit() {
  if (writes_.empty()) {
    return true;
  }
  const bool committed = store_->kv_->Commit(writes_);
  writes_.clear();
  return committed;
}

void IndexedDbBackingStore::Transaction::Rollback() {
  writes_.clear();
}

int64_t IndexedDbBackingStore::Transaction::WriteSize() const {
  int64_t size = 0;
  for (const auto& [key, value] : writes_) {
    if (value) {
      size += static_cast<int64_t>(key.size() + value->size());
    }
  }
  return size;
}

std::optional<std::string> IndexedDbBackingStore::Transaction::Get(
    const std::string& key) {
  if (const auto written = writes_.find(key); written != writes_.end()) {
    return written->second;
  }
  return store_->kv_->Get(key);
}

std::optional<KvStore::Entry> IndexedDbBackingStore::Transaction::Seek(
    const std::string& key,
    const std::string& lower,
    const std::string& upper_bound,
    bool reverse) {
  std::string from = key;
  std::string upper = upper_bound;
  while (true) {
    std::optional<KvStore::Entry> base =
        store_->kv_->Seek(from, lower, upper, reverse);
    // The nearest of what this wrote, in the same direction.
    KvBatch::const_iterator written = writes_.end();
    if (!reverse) {
      written = writes_.lower_bound(std::max(from, lower));
      if (written != writes_.end() && written->first >= upper) {
        written = writes_.end();
      }
    } else {
      auto after = writes_.upper_bound(std::min(from, upper));
      while (after != writes_.begin()) {
        --after;
        if (after->first < lower) {
          break;
        }
        if (after->first < upper && after->first <= from) {
          written = after;
          break;
        }
      }
    }
    bool use_written = false;
    if (written != writes_.end()) {
      if (!base) {
        use_written = true;
      } else if (written->first == base->key) {
        use_written = true;
      } else {
        use_written =
            reverse ? written->first > base->key : written->first < base->key;
      }
    }
    if (!use_written) {
      return base;
    }
    if (written->second) {
      return KvStore::Entry{written->first, *written->second};
    }
    // Removed: on past it.
    if (!reverse) {
      from = written->first + '\0';
    } else {
      upper = written->first;
      from = written->first;
    }
  }
}

void IndexedDbBackingStore::Transaction::Put(std::string key,
                                             std::string value) {
  writes_[std::move(key)] = std::move(value);
}

void IndexedDbBackingStore::Transaction::Remove(std::string key) {
  writes_[std::move(key)] = std::nullopt;
}

void IndexedDbBackingStore::Transaction::RemoveRange(const std::string& lower,
                                                     const std::string& upper) {
  std::vector<std::string> keys;
  std::string from = lower;
  while (std::optional<KvStore::Entry> entry =
             Seek(from, lower, upper, /*reverse=*/false)) {
    from = entry->key + '\0';
    keys.push_back(std::move(entry->key));
  }
  for (std::string& key : keys) {
    Remove(std::move(key));
  }
}

bool IndexedDbBackingStore::Transaction::SetDatabaseVersion(
    int64_t version,
    int64_t max_object_store_id) {
  const std::optional<std::string> value =
      Get(DatabaseMetadataKey(database_id_));
  if (!value) {
    return false;
  }
  base::PickleIterator iterator =
      base::PickleIterator::WithData(base::as_byte_span(*value));
  int schema = 0;
  std::u16string name;
  if (!iterator.ReadInt(&schema) || !iterator.ReadString16(&name)) {
    return false;
  }
  Put(DatabaseMetadataKey(database_id_),
      DatabaseMetadataValue(name, version, max_object_store_id));
  return true;
}

bool IndexedDbBackingStore::Transaction::CreateObjectStore(
    const IndexedDbObjectStoreMetadata& store) {
  // A key generator starts at 1.
  Put(ObjectStoreMetadataKey(database_id_, store.id),
      ObjectStoreMetadataValue(store, /*key_generator_current_number=*/1));
  return true;
}

bool IndexedDbBackingStore::Transaction::UpdateObjectStore(
    const IndexedDbObjectStoreMetadata& store) {
  Put(ObjectStoreMetadataKey(database_id_, store.id),
      ObjectStoreMetadataValue(store, GetKeyGeneratorCurrentNumber(store.id)));
  return true;
}

bool IndexedDbBackingStore::Transaction::DeleteObjectStore(
    int64_t object_store_id) {
  for (const std::string& prefix :
       {RecordPrefix(database_id_, object_store_id),
        IndexEntriesOfStorePrefix(database_id_, object_store_id),
        BackReferencePrefix(database_id_, object_store_id),
        IndexMetadataPrefix(database_id_, object_store_id)}) {
    RemoveRange(prefix, PrefixEnd(prefix));
  }
  Remove(ObjectStoreMetadataKey(database_id_, object_store_id));
  return true;
}

bool IndexedDbBackingStore::Transaction::CreateIndex(
    int64_t object_store_id,
    const IndexedDbIndexMetadata& index) {
  Put(IndexMetadataKey(database_id_, object_store_id, index.id),
      IndexMetadataValue(index));
  return true;
}

bool IndexedDbBackingStore::Transaction::DeleteIndex(int64_t object_store_id,
                                                     int64_t index_id) {
  const std::string prefix =
      IndexEntryPrefix(database_id_, object_store_id, index_id);
  RemoveRange(prefix, PrefixEnd(prefix));
  Remove(IndexMetadataKey(database_id_, object_store_id, index_id));
  return true;
}

std::optional<std::string> IndexedDbBackingStore::Transaction::GetRecord(
    int64_t object_store_id,
    const IDBKey& key) {
  return Get(RecordPrefix(database_id_, object_store_id) +
             EncodeSortableIDBKey(key));
}

bool IndexedDbBackingStore::Transaction::KeyExistsInObjectStore(
    int64_t object_store_id,
    const IDBKey& key) {
  return GetRecord(object_store_id, key).has_value();
}

void IndexedDbBackingStore::Transaction::RemoveIndexEntries(
    int64_t object_store_id,
    const IDBKey& key) {
  const std::string encoded = EncodeSortableIDBKey(key);
  const std::string back_reference =
      BackReferencePrefix(database_id_, object_store_id) + encoded;
  for (const auto& [index_id, index_key] :
       ReadBackReference(Get(back_reference))) {
    Remove(IndexEntryPrefix(database_id_, object_store_id, index_id) +
           index_key + encoded);
  }
  Remove(back_reference);
}

void IndexedDbBackingStore::Transaction::PutRecord(int64_t object_store_id,
                                                   const IDBKey& key,
                                                   const std::string& value) {
  RemoveIndexEntries(object_store_id, key);
  Put(RecordPrefix(database_id_, object_store_id) + EncodeSortableIDBKey(key),
      value);
}

void IndexedDbBackingStore::Transaction::PutIndexEntries(
    int64_t object_store_id,
    int64_t index_id,
    const IDBKey& primary_key,
    const std::vector<IDBKey>& index_keys) {
  if (index_keys.empty()) {
    return;
  }
  const std::string encoded = EncodeSortableIDBKey(primary_key);
  const std::string back_reference =
      BackReferencePrefix(database_id_, object_store_id) + encoded;
  IndexEntries entries = ReadBackReference(Get(back_reference));
  const std::string prefix =
      IndexEntryPrefix(database_id_, object_store_id, index_id);
  for (const IDBKey& index_key : index_keys) {
    std::string encoded_index_key = EncodeSortableIDBKey(index_key);
    Put(prefix + encoded_index_key + encoded, std::string());
    entries.emplace_back(index_id, std::move(encoded_index_key));
  }
  Put(back_reference, BackReferenceValue(entries));
}

uint32_t IndexedDbBackingStore::Transaction::DeleteRange(
    int64_t object_store_id,
    const IndexedDbKeyRange& range) {
  const std::string prefix = RecordPrefix(database_id_, object_store_id);
  const auto [lower, upper] = Bounds(prefix, range);
  std::vector<std::string> keys;
  std::string from = lower;
  while (std::optional<KvStore::Entry> entry =
             Seek(from, lower, upper, /*reverse=*/false)) {
    from = entry->key + '\0';
    keys.push_back(std::move(entry->key));
  }
  for (std::string& key : keys) {
    const IDBKey record_key =
        DecodeSortableIDBKey(std::string_view(key).substr(prefix.size()));
    if (record_key.IsValid()) {
      RemoveIndexEntries(object_store_id, record_key);
    }
    Remove(std::move(key));
  }
  return static_cast<uint32_t>(keys.size());
}

void IndexedDbBackingStore::Transaction::ClearObjectStore(
    int64_t object_store_id) {
  for (const std::string& prefix :
       {RecordPrefix(database_id_, object_store_id),
        IndexEntriesOfStorePrefix(database_id_, object_store_id),
        BackReferencePrefix(database_id_, object_store_id)}) {
    RemoveRange(prefix, PrefixEnd(prefix));
  }
}

bool IndexedDbBackingStore::Transaction::IndexKeyExistsWithOtherPrimaryKey(
    int64_t object_store_id,
    int64_t index_id,
    const IDBKey& index_key,
    const IDBKey& primary_key) {
  const std::string prefix =
      IndexEntryPrefix(database_id_, object_store_id, index_id) +
      EncodeSortableIDBKey(index_key);
  const std::string upper = prefix + kPastKey;
  const std::string own = prefix + EncodeSortableIDBKey(primary_key);
  std::string from = prefix;
  while (std::optional<KvStore::Entry> entry =
             Seek(from, prefix, upper, /*reverse=*/false)) {
    if (entry->key != own) {
      return true;
    }
    from = entry->key + '\0';
  }
  return false;
}

int64_t IndexedDbBackingStore::Transaction::GetKeyGeneratorCurrentNumber(
    int64_t object_store_id) {
  const std::optional<std::string> value =
      Get(ObjectStoreMetadataKey(database_id_, object_store_id));
  if (!value) {
    return -1;
  }
  base::PickleIterator iterator =
      base::PickleIterator::WithData(base::as_byte_span(*value));
  std::u16string name;
  IDBKeyPath key_path;
  bool auto_increment = false;
  int64_t max_index_id = 0;
  int64_t current = 0;
  if (!iterator.ReadString16(&name) || !ReadKeyPath(iterator, key_path) ||
      !iterator.ReadBool(&auto_increment) ||
      !iterator.ReadInt64(&max_index_id) || !iterator.ReadInt64(&current)) {
    return -1;
  }
  return current;
}

void IndexedDbBackingStore::Transaction::MaybeUpdateKeyGeneratorCurrentNumber(
    int64_t object_store_id,
    int64_t new_number,
    bool was_generated) {
  const std::string key = ObjectStoreMetadataKey(database_id_, object_store_id);
  const std::optional<std::string> value = Get(key);
  if (!value) {
    return;
  }
  base::PickleIterator iterator =
      base::PickleIterator::WithData(base::as_byte_span(*value));
  IndexedDbObjectStoreMetadata store;
  int64_t current = 0;
  if (!iterator.ReadString16(&store.name) ||
      !ReadKeyPath(iterator, store.key_path) ||
      !iterator.ReadBool(&store.auto_increment) ||
      !iterator.ReadInt64(&store.max_index_id) ||
      !iterator.ReadInt64(&current)) {
    return;
  }
  if (!was_generated && new_number <= current) {
    return;
  }
  Put(key, ObjectStoreMetadataValue(store, new_number));
}

std::unique_ptr<IndexedDbBackingStore::Cursor>
IndexedDbBackingStore::Transaction::OpenCursor(
    int64_t object_store_id,
    int64_t index_id,
    const IndexedDbKeyRange& range,
    IndexedDbCursorDirection direction,
    bool key_only) {
  const std::string prefix =
      index_id == IndexedDbIndexMetadata::kInvalidId
          ? RecordPrefix(database_id_, object_store_id)
          : IndexEntryPrefix(database_id_, object_store_id, index_id);
  auto [lower, upper] = Bounds(prefix, range);
  auto cursor = std::unique_ptr<Cursor>(
      new Cursor(*this, object_store_id, index_id, std::move(lower),
                 std::move(upper), direction, key_only));
  const bool found = cursor->IsReverse() ? cursor->FirstFrom(cursor->upper_)
                                         : cursor->FirstFrom(cursor->lower_);
  if (!found) {
    return nullptr;
  }
  return cursor;
}

uint32_t IndexedDbBackingStore::Transaction::Count(
    int64_t object_store_id,
    int64_t index_id,
    const IndexedDbKeyRange& range) {
  const std::string prefix =
      index_id == IndexedDbIndexMetadata::kInvalidId
          ? RecordPrefix(database_id_, object_store_id)
          : IndexEntryPrefix(database_id_, object_store_id, index_id);
  const auto [lower, upper] = Bounds(prefix, range);
  uint32_t count = 0;
  std::string from = lower;
  while (std::optional<KvStore::Entry> entry =
             Seek(from, lower, upper, /*reverse=*/false)) {
    from = entry->key + '\0';
    ++count;
  }
  return count;
}

IndexedDbBackingStore::Cursor::Cursor(Transaction& transaction,
                                      int64_t object_store_id,
                                      int64_t index_id,
                                      std::string lower,
                                      std::string upper,
                                      IndexedDbCursorDirection direction,
                                      bool key_only)
    : transaction_(transaction),
      object_store_id_(object_store_id),
      index_id_(index_id),
      lower_(std::move(lower)),
      upper_(std::move(upper)),
      direction_(direction),
      key_only_(key_only) {}

IndexedDbBackingStore::Cursor::~Cursor() = default;

bool IndexedDbBackingStore::Cursor::IsReverse() const {
  return direction_ == IndexedDbCursorDirection::kPrev ||
         direction_ == IndexedDbCursorDirection::kPrevNoDuplicate;
}

bool IndexedDbBackingStore::Cursor::IsUnique() const {
  return direction_ == IndexedDbCursorDirection::kNextNoDuplicate ||
         direction_ == IndexedDbCursorDirection::kPrevNoDuplicate;
}

bool IndexedDbBackingStore::Cursor::FirstFrom(const std::string& from) {
  std::optional<KvStore::Entry> entry;
  if (!IsReverse()) {
    entry = transaction_->Seek(from, lower_, upper_, /*reverse=*/false);
  } else {
    // The last entry before `from`.
    const std::string& upper = from < upper_ ? from : upper_;
    entry = transaction_->Seek(upper, lower_, upper, /*reverse=*/true);
    if (entry && IsIndex() && IsUnique()) {
      return Take(FirstOfIndexKey(entry->key));
    }
  }
  if (!entry) {
    return false;
  }
  return Take(entry->key);
}

std::string IndexedDbBackingStore::Cursor::FirstOfIndexKey(
    const std::string& kv_key) {
  const size_t prefix_size = 1 + 8 + 1 + 8 + 8;
  std::optional<std::pair<IDBKey, size_t>> index_key =
      DecodeSortableIDBKeyPrefix(std::string_view(kv_key).substr(prefix_size));
  if (!index_key) {
    return kv_key;
  }
  const std::string key_prefix =
      kv_key.substr(0, prefix_size + index_key->second);
  std::optional<KvStore::Entry> first = transaction_->Seek(
      key_prefix, std::max(key_prefix, lower_), upper_, /*reverse=*/false);
  return first ? first->key : kv_key;
}

bool IndexedDbBackingStore::Cursor::Take(const std::string& kv_key) {
  // The database's prefix, the type, the object store and, of an index, the
  // index.
  const size_t prefix_size = IsIndex() ? 1 + 8 + 1 + 8 + 8 : 1 + 8 + 1 + 8;
  const std::string_view rest = std::string_view(kv_key).substr(prefix_size);
  if (!IsIndex()) {
    key_ = DecodeSortableIDBKey(rest);
    primary_key_ = key_;
  } else {
    std::optional<std::pair<IDBKey, size_t>> index_key =
        DecodeSortableIDBKeyPrefix(rest);
    if (!index_key) {
      return false;
    }
    key_ = std::move(index_key->first);
    primary_key_ = DecodeSortableIDBKey(rest.substr(index_key->second));
  }
  if (!key_.IsValid() || !primary_key_.IsValid()) {
    return false;
  }
  position_ = kv_key;
  value_.clear();
  if (!key_only_) {
    std::optional<std::string> value =
        transaction_->GetRecord(object_store_id_, primary_key_);
    if (!value) {
      return false;
    }
    value_ = std::move(*value);
  }
  return true;
}

bool IndexedDbBackingStore::Cursor::Continue(const IDBKey& key,
                                             const IDBKey& primary_key) {
  const size_t prefix_size = IsIndex() ? 1 + 8 + 1 + 8 + 8 : 1 + 8 + 1 + 8;
  const std::string prefix = position_.substr(0, prefix_size);
  if (key.IsValid()) {
    std::string target = prefix + EncodeSortableIDBKey(key);
    if (!IsReverse()) {
      if (primary_key.IsValid()) {
        target += EncodeSortableIDBKey(primary_key);
      }
      if (target <= position_) {
        target = position_ + '\0';
      }
      return FirstFrom(target);
    }
    // At or before the key, and the primary key: before every entry past
    // them.
    if (primary_key.IsValid()) {
      target += EncodeSortableIDBKey(primary_key);
      target += '\0';
    } else {
      target += kPastKey;
    }
    if (target > position_) {
      target = position_;
    }
    return FirstFrom(target);
  }
  if (!IsReverse()) {
    if (IsIndex() && IsUnique()) {
      // Past every entry of this index key.
      return FirstFrom(prefix + EncodeSortableIDBKey(key_) + kPastKey);
    }
    return FirstFrom(position_ + '\0');
  }
  if (IsIndex() && IsUnique()) {
    // Before the first entry of this index key.
    return FirstFrom(prefix + EncodeSortableIDBKey(key_));
  }
  return FirstFrom(position_);
}

void IndexedDbBackingStore::Cursor::SetPosition(Position position) {
  position_ = std::move(position.kv_key);
  key_ = std::move(position.key);
  primary_key_ = std::move(position.primary_key);
  value_.clear();
}

bool IndexedDbBackingStore::Cursor::Advance(uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (!Continue()) {
      return false;
    }
  }
  return true;
}

}  // namespace mitto
