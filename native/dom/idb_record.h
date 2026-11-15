// IndexedDB's IDBRecord, as Blink's: a record that getAllRecords() of an
// object store or an index gives, with its primary key, its value, and its
// key, which is the index key of an index's record and the primary key of an
// object store's. Each attribute gives what it gave first every time after,
// as Blink's [CachedAttribute]s, which never become dirty again, keep it.
//
// IDBRecordArray::ToV8(), which makes these of getAllRecords()' results, is
// here too.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_record.h
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_record.cc
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_record_array.cc

#ifndef MITTO_DOM_IDB_RECORD_H_
#define MITTO_DOM_IDB_RECORD_H_

#include <optional>

#include "mitto/bindings/script_wrappable.h"
#include "mitto/indexed_db_key.h"
#include "mitto/indexed_db_messages.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-traced-handle.h"

namespace mitto {

class IDBRecord final : public bindings::ScriptWrappable {
 public:
  // `index_key` is nullopt for an object store's record.
  IDBRecord(IDBKey primary_key,
            IndexedDbReturnValue value,
            std::optional<IDBKey> index_key);
  IDBRecord(const IDBRecord&) = delete;
  IDBRecord& operator=(const IDBRecord&) = delete;
  ~IDBRecord() override;

  const bindings::WrapperTypeInfo* GetWrapperTypeInfo() const override;
  void Trace(cppgc::Visitor* visitor) const override;

  // The members of the interface.
  v8::Local<v8::Value> Key();
  v8::Local<v8::Value> PrimaryKey();
  v8::Local<v8::Value> Value();

 private:
  const IDBKey primary_key_;
  const IndexedDbReturnValue value_;
  const std::optional<IDBKey> index_key_;
  v8::TracedReference<v8::Value> cached_key_;
  v8::TracedReference<v8::Value> cached_primary_key_;
  v8::TracedReference<v8::Value> cached_value_;
};

}  // namespace mitto

#endif  // MITTO_DOM_IDB_RECORD_H_
