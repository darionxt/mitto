#include "mitto/dom/idb_metadata.h"

#include <utility>

namespace mitto {

IDBIndexMetadata::IDBIndexMetadata() = default;

IDBIndexMetadata::IDBIndexMetadata(const std::u16string& name,
                                   int64_t id,
                                   const IDBKeyPath& key_path,
                                   bool unique,
                                   bool multi_entry)
    : name(name),
      id(id),
      key_path(key_path),
      unique(unique),
      multi_entry(multi_entry) {}

IDBIndexMetadata::~IDBIndexMetadata() = default;

// static
scoped_refptr<IDBIndexMetadata> IDBIndexMetadata::FromMessage(
    const IndexedDbIndexMetadata& metadata) {
  return base::MakeRefCounted<IDBIndexMetadata>(
      metadata.name, metadata.id, metadata.key_path, metadata.unique,
      metadata.multi_entry);
}

IndexedDbIndexMetadata IDBIndexMetadata::ToMessage() const {
  IndexedDbIndexMetadata metadata;
  metadata.id = id;
  metadata.name = name;
  metadata.key_path = key_path;
  metadata.unique = unique;
  metadata.multi_entry = multi_entry;
  return metadata;
}

IDBObjectStoreMetadata::IDBObjectStoreMetadata() = default;

IDBObjectStoreMetadata::IDBObjectStoreMetadata(const std::u16string& name,
                                               int64_t id,
                                               const IDBKeyPath& key_path,
                                               bool auto_increment)
    : name(name), id(id), key_path(key_path), auto_increment(auto_increment) {}

IDBObjectStoreMetadata::~IDBObjectStoreMetadata() = default;

// static
scoped_refptr<IDBObjectStoreMetadata> IDBObjectStoreMetadata::FromMessage(
    const IndexedDbObjectStoreMetadata& metadata) {
  scoped_refptr<IDBObjectStoreMetadata> store =
      base::MakeRefCounted<IDBObjectStoreMetadata>(metadata.name, metadata.id,
                                                   metadata.key_path,
                                                   metadata.auto_increment);
  store->max_index_id = metadata.max_index_id;
  for (const auto& [id, index] : metadata.indexes) {
    store->indexes.emplace(id, IDBIndexMetadata::FromMessage(index));
  }
  return store;
}

scoped_refptr<IDBObjectStoreMetadata> IDBObjectStoreMetadata::CreateCopy()
    const {
  scoped_refptr<IDBObjectStoreMetadata> copy =
      base::MakeRefCounted<IDBObjectStoreMetadata>(name, id, key_path,
                                                   auto_increment);
  copy->max_index_id = max_index_id;
  for (const auto& [index_id, index] : indexes) {
    copy->indexes.emplace(index_id, base::MakeRefCounted<IDBIndexMetadata>(
                                        index->name, index->id, index->key_path,
                                        index->unique, index->multi_entry));
  }
  return copy;
}

IDBDatabaseMetadata::IDBDatabaseMetadata() = default;
IDBDatabaseMetadata::IDBDatabaseMetadata(const IDBDatabaseMetadata&) = default;
IDBDatabaseMetadata& IDBDatabaseMetadata::operator=(
    const IDBDatabaseMetadata&) = default;
IDBDatabaseMetadata::~IDBDatabaseMetadata() = default;

// static
IDBDatabaseMetadata IDBDatabaseMetadata::FromMessage(
    const IndexedDbDatabaseMetadata& metadata) {
  IDBDatabaseMetadata result;
  result.name = metadata.name;
  result.version = metadata.version;
  result.max_object_store_id = metadata.max_object_store_id;
  for (const auto& [id, store] : metadata.object_stores) {
    result.object_stores.emplace(id,
                                 IDBObjectStoreMetadata::FromMessage(store));
  }
  return result;
}

void IDBDatabaseMetadata::CopyFrom(const IDBDatabaseMetadata& metadata) {
  name = metadata.name;
  version = metadata.version;
  max_object_store_id = metadata.max_object_store_id;
}

}  // namespace mitto
