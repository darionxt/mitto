// IndexedDB's IDBObjectStore, as Blink's: an object store in a transaction,
// whose metadata it shares with its database's, and whose writes, reads and
// cursors go to the Session. A write is serialized as a structured clone,
// with its primary key and its keys in each index taken from a copy of it,
// so that a page's getters run once. An upgrade can rename it, and make and
// delete its indexes; a new index is filled from a cursor over the store's
// records, which the Session runs before the transaction's other tasks. It
// gives the same IDBIndex for a name each time, and puts its metadata and
// its indexes' back if the upgrade aborts.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_object_store.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_object_store.cc

#ifndef MITTO_DOM_IDB_OBJECT_STORE_H_
#define MITTO_DOM_IDB_OBJECT_STORE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/heap_vector.h"
#include "mitto/dom/idb_metadata.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

namespace bindings {
class ExceptionState;
}  // namespace bindings

class DOMStringList;
class IDBDatabase;
class IDBIndex;
class IDBRequest;
class IDBTransaction;

class IDBObjectStore final : public bindings::ScriptWrappable {
 public:
  IDBObjectStore(scoped_refptr<IDBObjectStoreMetadata> metadata,
                 IDBTransaction* transaction);
  ~IDBObjectStore() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Name() const { return metadata_->name; }
  void SetName(const std::u16string& name,
               bindings::ExceptionState& exception_state);
  // The same object each time, as [CachedAttribute=IsKeyPathDirty], which is
  // never dirty, keeps it.
  v8::Local<v8::Value> KeyPath();
  DOMStringList* IndexNames() const;
  IDBTransaction* Transaction() const { return transaction_.Get(); }
  bool AutoIncrement() const { return metadata_->auto_increment; }
  IDBRequest* Put(v8::Local<v8::Value> value,
                  std::optional<v8::Local<v8::Value>> key,
                  bindings::ExceptionState& exception_state);
  IDBRequest* Add(v8::Local<v8::Value> value,
                  std::optional<v8::Local<v8::Value>> key,
                  bindings::ExceptionState& exception_state);
  IDBRequest* Delete(v8::Local<v8::Value> key,
                     bindings::ExceptionState& exception_state);
  IDBRequest* Clear(bindings::ExceptionState& exception_state);
  IDBRequest* Get(v8::Local<v8::Value> key,
                  bindings::ExceptionState& exception_state);
  IDBRequest* GetKey(v8::Local<v8::Value> key,
                     bindings::ExceptionState& exception_state);
  IDBRequest* GetAll(v8::Local<v8::Value> query_or_options,
                     std::optional<uint32_t> count,
                     bindings::ExceptionState& exception_state);
  IDBRequest* GetAllKeys(v8::Local<v8::Value> query_or_options,
                         std::optional<uint32_t> count,
                         bindings::ExceptionState& exception_state);
  IDBRequest* GetAllRecords(const bindings::IDBGetAllOptions& options,
                            bindings::ExceptionState& exception_state);
  IDBRequest* Count(v8::Local<v8::Value> key,
                    bindings::ExceptionState& exception_state);
  IDBRequest* OpenCursor(v8::Local<v8::Value> range,
                         const std::u16string& direction,
                         bindings::ExceptionState& exception_state);
  IDBRequest* OpenKeyCursor(v8::Local<v8::Value> range,
                            const std::u16string& direction,
                            bindings::ExceptionState& exception_state);
  IDBIndex* Index(const std::u16string& name,
                  bindings::ExceptionState& exception_state);
  IDBIndex* CreateIndex(const std::u16string& name,
                        const bindings::DOMStringOrDOMStringSequence& key_path,
                        const bindings::IDBIndexParameters& options,
                        bindings::ExceptionState& exception_state);
  void DeleteIndex(const std::u16string& name,
                   bindings::ExceptionState& exception_state);

  // What the rest of IndexedDB uses.
  const IDBObjectStoreMetadata& Metadata() const { return *metadata_; }
  int64_t Id() const { return metadata_->id; }
  // Puts `value` with `key`, or without one for a key generator or in-line
  // keys, for a request of `source`, as IDBCursor.update() does too.
  IDBRequest* DoPut(IndexedDbPutMode put_mode,
                    bindings::ScriptWrappable* source,
                    v8::Local<v8::Value> value,
                    const IDBKey* key,
                    bindings::ExceptionState& exception_state);
  // A cursor over `range`, or every record for nullopt, which createIndex()
  // opens as a preemptive task.
  IDBRequest* OpenCursor(std::optional<IndexedDbKeyRange> range,
                         IndexedDbCursorDirection direction,
                         IndexedDbTaskType task_type);
  // Whether its transaction, an upgrade, made it.
  bool IsNewlyCreated() const;
  bool IsDeleted() const { return deleted_; }
  void MarkDeleted();
  // Lets go of the IDBIndex objects it gave, once its transaction has, so
  // that the collector can take those that no script holds.
  void ClearIndexCache();
  // Puts back the metadata that its transaction kept, when that aborts, and
  // its indexes' that were there before, while those that the transaction
  // made stay deleted.
  void RevertMetadata(scoped_refptr<IDBObjectStoreMetadata> old_metadata);
  // Puts back the metadata of an index that the transaction deleted, from
  // what RevertMetadata() put back.
  void RevertDeletedIndexMetadata(IDBIndex& deleted_index);
  // IDBIndex's name setter.
  bool ContainsIndex(const std::u16string& name) const {
    return FindIndexId(name) != IDBIndexMetadata::kInvalidId;
  }
  void RenameIndex(int64_t index_id, const std::u16string& new_name);

 private:
  IDBDatabase& db() const;
  IDBIndex* CreateIndex(const std::u16string& name,
                        const IDBKeyPath& key_path,
                        const bindings::IDBIndexParameters& options,
                        bindings::ExceptionState& exception_state);
  // put() and add(), with a key that a page gave, or none.
  IDBRequest* DoPut(IndexedDbPutMode put_mode,
                    v8::Local<v8::Value> value,
                    std::optional<v8::Local<v8::Value>> key_value,
                    bindings::ExceptionState& exception_state);
  // get() and getKey().
  IDBRequest* GetInternal(v8::Local<v8::Value> key,
                          bool key_only,
                          bindings::ExceptionState& exception_state);
  IDBRequest* CreateGetAllRequest(const bindings::IDBGetAllOptions& options,
                                  IndexedDbGetAllResultType result_type,
                                  bindings::ExceptionState& exception_state);
  int64_t FindIndexId(const std::u16string& name) const;
  // index()'s cache: the IDBIndex it gave for `name`, or null.
  IDBIndex* FindCachedIndex(const std::u16string& name) const;
  void EraseCachedIndex(const std::u16string& name);

  // Shared with the database's metadata.
  scoped_refptr<IDBObjectStoreMetadata> metadata_;
  const cppgc::Member<IDBTransaction> transaction_;
  bool deleted_ = false;
  v8::TracedReference<v8::Value> cached_key_path_;
  // The IDBIndex objects that index() and createIndex() gave, by name, which
  // index() gives again, as the spec has it. index() throws once the
  // transaction has finished, so they go then.
  std::vector<std::u16string> index_names_;
  HeapVector<IDBIndex> indexes_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_OBJECT_STORE_H_
