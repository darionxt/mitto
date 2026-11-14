// IndexedDB's IDBIndex, as Blink's: an index of an object store in a
// transaction, whose metadata it shares with its object store's, and whose
// reads and cursors go to the Session as the object store's do, with the
// index's ID. Its object store gives the same IDBIndex for a name each time;
// an upgrade can rename or delete it, and puts its metadata back if it
// aborts.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_index.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_index.cc

#ifndef MITTO_DOM_IDB_INDEX_H_
#define MITTO_DOM_IDB_INDEX_H_

#include <stdint.h>

#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "mitto/bindings/dictionaries.h"
#include "mitto/bindings/script_wrappable.h"
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

class IDBDatabase;
class IDBObjectStore;
class IDBRequest;
class IDBTransaction;

class IDBIndex final : public bindings::ScriptWrappable {
 public:
  IDBIndex(scoped_refptr<IDBIndexMetadata> metadata,
           IDBObjectStore* object_store,
           IDBTransaction* transaction);
  ~IDBIndex() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  std::u16string Name() const { return metadata_->name; }
  void SetName(const std::u16string& name,
               bindings::ExceptionState& exception_state);
  IDBObjectStore* ObjectStore() const { return object_store_.Get(); }
  // The same object each time, as [CachedAttribute=IsKeyPathDirty], which is
  // never dirty, keeps it.
  v8::Local<v8::Value> KeyPath();
  bool MultiEntry() const { return metadata_->multi_entry; }
  bool Unique() const { return metadata_->unique; }
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

  // What the rest of IndexedDB uses.
  const IDBIndexMetadata& Metadata() const { return *metadata_; }
  int64_t Id() const { return metadata_->id; }
  void MarkDeleted();
  bool IsDeleted() const { return deleted_; }
  // Whether its transaction, an upgrade, made it, as its object store's
  // metadata had it when the upgrade began.
  bool IsNewlyCreated(
      const IDBObjectStoreMetadata& old_object_store_metadata) const;
  // Puts back the metadata that its transaction kept, when that aborts.
  void RevertMetadata(scoped_refptr<IDBIndexMetadata> old_metadata);

 private:
  IDBDatabase& db() const;
  // get() and getKey().
  IDBRequest* GetInternal(v8::Local<v8::Value> key,
                          bindings::ExceptionState& exception_state,
                          bool key_only);
  IDBRequest* CreateGetAllRequest(const bindings::IDBGetAllOptions& options,
                                  IndexedDbGetAllResultType result_type,
                                  bindings::ExceptionState& exception_state);

  scoped_refptr<IDBIndexMetadata> metadata_;
  const cppgc::Member<IDBObjectStore> object_store_;
  const cppgc::Member<IDBTransaction> transaction_;
  bool deleted_ = false;
  v8::TracedReference<v8::Value> cached_key_path_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_INDEX_H_
