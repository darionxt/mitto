#include "mitto/dom/idb_object_store.h"

#include <limits>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/memory/ref_counted.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_idb_object_store.h"
#include "mitto/dom/dom_string_list.h"
#include "mitto/dom/idb_any.h"
#include "mitto/dom/idb_cursor.h"
#include "mitto/dom/idb_database.h"
#include "mitto/dom/idb_get_all_options_helper.h"
#include "mitto/dom/idb_index.h"
#include "mitto/dom/idb_key_conversion.h"
#include "mitto/dom/idb_key_range.h"
#include "mitto/dom/idb_request.h"
#include "mitto/dom/idb_transaction.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {
namespace {

// Blink's IndexPopulator: the success listener of the request whose cursor
// createIndex() opens, which gives the Session the new index's keys of each
// record of the store, from its value, and tells it when there are no more,
// so that it goes back to the transaction's other tasks. Its request keeps
// it, and the transaction keeps the request.
class IndexPopulator final : public IDBRequest::NativeSuccessListener {
 public:
  IndexPopulator(IDBTransaction* transaction,
                 int64_t object_store_id,
                 scoped_refptr<const IDBObjectStoreMetadata> store_metadata,
                 scoped_refptr<const IDBIndexMetadata> index_metadata)
      : transaction_(transaction),
        object_store_id_(object_store_id),
        store_metadata_(std::move(store_metadata)),
        index_metadata_(std::move(index_metadata)) {
    DCHECK(index_metadata_);
  }
  ~IndexPopulator() override = default;

  void Trace(cppgc::Visitor* visitor) const override {
    IDBRequest::NativeSuccessListener::Trace(visitor);
    visitor->Trace(transaction_);
  }

  void Invoke(IDBRequest* request) override {
    if (!request->HasContext()) {
      return;
    }
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();

    IDBAny* const cursor_any = request->ResultAsAny();
    IDBCursor* cursor = nullptr;
    if (cursor_any &&
        cursor_any->GetType() == IDBAny::kIDBCursorWithValueType) {
      cursor = cursor_any->Cursor();
    }

    if (cursor && !cursor->IsDeleted()) {
      bindings::ExceptionState exception_state(isolate);
      cursor->Continue(IDBKey(), IDBKey(), exception_state);
      DCHECK(!exception_state.HadException());

      const IDBKey primary_key = cursor->IdbPrimaryKey();
      const v8::Local<v8::Value> value = cursor->Value();

      transaction_->SetIndexKeys(
          object_store_id_, primary_key,
          GenerateIndexKeysForValue(isolate, store_metadata_->key_path,
                                    index_metadata_->ToMessage(), value));
    } else {
      // Done: the Session goes back to the transaction's normal tasks.
      transaction_->SetIndexReady(object_store_id_);
      transaction_.Clear();
    }
  }

 private:
  cppgc::Member<IDBTransaction> transaction_;
  const int64_t object_store_id_;
  const scoped_refptr<const IDBObjectStoreMetadata> store_metadata_;
  const scoped_refptr<const IDBIndexMetadata> index_metadata_;
};

}  // namespace

IDBObjectStore::IDBObjectStore(scoped_refptr<IDBObjectStoreMetadata> metadata,
                               IDBTransaction* transaction)
    : metadata_(std::move(metadata)), transaction_(transaction) {
  DCHECK(metadata_);
  DCHECK(transaction_);
}

IDBObjectStore::~IDBObjectStore() = default;

const bindings::WrapperTypeInfo* IDBObjectStore::GetWrapperTypeInfo() const {
  return &bindings::V8IDBObjectStore::kWrapperTypeInfo;
}

void IDBObjectStore::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(transaction_);
  visitor->Trace(cached_key_path_);
  indexes_.Trace(visitor);
}

IDBDatabase& IDBObjectStore::db() const {
  return transaction_->db();
}

void IDBObjectStore::SetName(const std::u16string& name,
                             bindings::ExceptionState& exception_state) {
  if (!transaction_->IsVersionChange()) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        IDBDatabase::kNotVersionChangeTransactionErrorMessage);
    return;
  }
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return;
  }

  if (Name() == name) {
    return;
  }
  if (db().ContainsObjectStore(name)) {
    exception_state.ThrowDOMException(
        "ConstraintError", IDBDatabase::kObjectStoreNameTakenErrorMessage);
    return;
  }
  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return;
  }

  db().RenameObjectStore(Id(), name);
}

v8::Local<v8::Value> IDBObjectStore::KeyPath() {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  if (!cached_key_path_.IsEmpty()) {
    return cached_key_path_.Get(isolate);
  }
  v8::Local<v8::Value> key_path = IDBKeyPathToV8(isolate, Metadata().key_path);
  if (key_path.IsEmpty()) {
    key_path = v8::Undefined(isolate);
  }
  cached_key_path_.Reset(isolate, key_path);
  return key_path;
}

DOMStringList* IDBObjectStore::IndexNames() const {
  DOMStringList* const index_names =
      bindings::MakeGarbageCollected<DOMStringList>();
  for (const auto& [id, index] : Metadata().indexes) {
    index_names->Append(index->name);
  }
  index_names->Sort();
  return index_names;
}

IDBRequest* IDBObjectStore::Get(v8::Local<v8::Value> key,
                                bindings::ExceptionState& exception_state) {
  return GetInternal(key, /*key_only=*/false, exception_state);
}

IDBRequest* IDBObjectStore::GetKey(v8::Local<v8::Value> key,
                                   bindings::ExceptionState& exception_state) {
  return GetInternal(key, /*key_only=*/true, exception_state);
}

IDBRequest* IDBObjectStore::GetInternal(
    v8::Local<v8::Value> key,
    bool key_only,
    bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }
  std::optional<IndexedDbKeyRange> key_range;
  if (!IDBKeyRange::FromScriptValue(v8::Isolate::GetCurrent(), key,
                                    exception_state, &key_range)) {
    return nullptr;
  }
  if (!key_range) {
    exception_state.ThrowDOMException(
        "DataError", IDBDatabase::kNoKeyOrKeyRangeErrorMessage);
    return nullptr;
  }
  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  IDBRequest* const request = IDBRequest::Create(this, transaction_.Get());
  db().Get(transaction_->Id(), Id(), IDBIndexMetadata::kInvalidId,
           *std::move(key_range), key_only, request);
  return request;
}

IDBRequest* IDBObjectStore::GetAll(v8::Local<v8::Value> query_or_options,
                                   std::optional<uint32_t> count,
                                   bindings::ExceptionState& exception_state) {
  bindings::IDBGetAllOptions options;
  if (!IDBGetAllOptionsFromArgumentsOrDictionary(
          v8::Isolate::GetCurrent(), query_or_options,
          count.value_or(std::numeric_limits<uint32_t>::max()), exception_state,
          &options)) {
    return nullptr;
  }
  return CreateGetAllRequest(options, IndexedDbGetAllResultType::kValues,
                             exception_state);
}

IDBRequest* IDBObjectStore::GetAllKeys(
    v8::Local<v8::Value> query_or_options,
    std::optional<uint32_t> count,
    bindings::ExceptionState& exception_state) {
  bindings::IDBGetAllOptions options;
  if (!IDBGetAllOptionsFromArgumentsOrDictionary(
          v8::Isolate::GetCurrent(), query_or_options,
          count.value_or(std::numeric_limits<uint32_t>::max()), exception_state,
          &options)) {
    return nullptr;
  }
  return CreateGetAllRequest(options, IndexedDbGetAllResultType::kKeys,
                             exception_state);
}

IDBRequest* IDBObjectStore::GetAllRecords(
    const bindings::IDBGetAllOptions& options,
    bindings::ExceptionState& exception_state) {
  return CreateGetAllRequest(options, IndexedDbGetAllResultType::kRecords,
                             exception_state);
}

IDBRequest* IDBObjectStore::Add(v8::Local<v8::Value> value,
                                std::optional<v8::Local<v8::Value>> key,
                                bindings::ExceptionState& exception_state) {
  return DoPut(IndexedDbPutMode::kAddOnly, value, key, exception_state);
}

IDBRequest* IDBObjectStore::Put(v8::Local<v8::Value> value,
                                std::optional<v8::Local<v8::Value>> key,
                                bindings::ExceptionState& exception_state) {
  return DoPut(IndexedDbPutMode::kAddOrUpdate, value, key, exception_state);
}

IDBRequest* IDBObjectStore::DoPut(IndexedDbPutMode put_mode,
                                  v8::Local<v8::Value> value,
                                  std::optional<v8::Local<v8::Value>> key_value,
                                  bindings::ExceptionState& exception_state) {
  // A key of undefined is no key, as one that is not given.
  std::optional<IDBKey> key;
  if (key_value && !(*key_value)->IsUndefined()) {
    IDBKey converted;
    if (!CreateIDBKeyFromValue(v8::Isolate::GetCurrent(), *key_value,
                               exception_state, &converted)) {
      return nullptr;
    }
    key = std::move(converted);
  }
  return DoPut(put_mode, this, value, key ? &*key : nullptr, exception_state);
}

IDBRequest* IDBObjectStore::DoPut(IndexedDbPutMode put_mode,
                                  bindings::ScriptWrappable* source,
                                  v8::Local<v8::Value> value,
                                  const IDBKey* key,
                                  bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }
  if (transaction_->IsReadOnly()) {
    exception_state.ThrowDOMException(
        "ReadOnlyError", IDBDatabase::kTransactionReadOnlyErrorMessage);
    return nullptr;
  }

  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  DCHECK(isolate->InContext());
  // IDBValueWrapper: the transaction is inactive while a page's getters run.
  // What fails throws on the isolate, so nothing is sent.
  transaction_->SetActiveDuringSerialization(false);
  std::optional<SerializedScriptValue> serialized =
      SerializedScriptValue::Serialize(isolate, value);
  transaction_->SetActiveDuringSerialization(true);
  if (!serialized) {
    return nullptr;
  }
  // Blink's serializer for storage throws this where it meets the module;
  // here it comes once the rest of the value has been serialized.
  if (serialized->IsLockedToAgentCluster()) {
    exception_state.ThrowDOMException(
        "DataCloneError",
        "A WebAssembly.Module can not be serialized for storage.");
    return nullptr;
  }

  // Keys are taken from a copy, so that a page's getters do not run again,
  // which is made only once one is needed.
  v8::Local<v8::Value> clone;

  const IDBKeyPath& key_path = Metadata().key_path;
  const bool uses_in_line_keys = !key_path.IsNull();
  const bool has_key_generator = AutoIncrement();

  if (put_mode != IndexedDbPutMode::kCursorUpdate && uses_in_line_keys && key) {
    exception_state.ThrowDOMException("DataError",
                                      "The object store uses in-line keys and "
                                      "the key parameter was provided.");
    return nullptr;
  }

  // The key that the key path takes from the value, which lives as long as
  // the call.
  std::optional<IDBKey> key_path_key;

  // IDBCursor's check, which needs the copy.
  if (put_mode == IndexedDbPutMode::kCursorUpdate && uses_in_line_keys) {
    DCHECK(key);
    clone = serialized->Deserialize(isolate);
    if (!CreateIDBKeyFromValueAndKeyPath(isolate, clone, key_path,
                                         exception_state, &key_path_key)) {
      return nullptr;
    }
    if (!key_path_key || !key_path_key->Equals(*key)) {
      exception_state.ThrowDOMException(
          "DataError",
          "The effective object store of this cursor uses in-line keys and "
          "evaluating the key path of the value parameter results in a "
          "different value than the cursor's effective key.");
      return nullptr;
    }
  }

  if (!uses_in_line_keys && !has_key_generator && !key) {
    exception_state.ThrowDOMException("DataError",
                                      "The object store uses out-of-line keys "
                                      "and has no key generator and the key "
                                      "parameter was not provided.");
    return nullptr;
  }

  if (uses_in_line_keys) {
    if (clone.IsEmpty()) {
      // update() made the copy above.
      DCHECK(put_mode != IndexedDbPutMode::kCursorUpdate);
      clone = serialized->Deserialize(isolate);
      if (!CreateIDBKeyFromValueAndKeyPath(isolate, clone, key_path,
                                           exception_state, &key_path_key)) {
        return nullptr;
      }
      if (key_path_key && !key_path_key->IsValid()) {
        exception_state.ThrowDOMException(
            "DataError",
            "Evaluating the object store's key path yielded a value that is "
            "not a valid key.");
        return nullptr;
      }
    } else {
      DCHECK(put_mode == IndexedDbPutMode::kCursorUpdate);
      DCHECK(key_path_key && key_path_key->IsValid());
    }

    // put() and add() may find no key, which a key generator makes.
    if (!key_path_key) {
      if (!has_key_generator) {
        exception_state.ThrowDOMException("DataError",
                                          "Evaluating the object store's key "
                                          "path did not yield a value.");
        return nullptr;
      }

      // The Session makes the key, so that a database that several Pages
      // share gives each one once, and the key goes into the value when it is
      // read back, which has to be possible.
      if (!CanInjectIDBKeyIntoScriptValue(isolate, clone, key_path)) {
        exception_state.ThrowDOMException("DataError",
                                          "A generated key could not be "
                                          "inserted into the value.");
        return nullptr;
      }
    }

    if (key_path_key) {
      key = &*key_path_key;
    }
  }

  if (key && !key->IsValid()) {
    exception_state.ThrowDOMException("DataError",
                                      IDBDatabase::kNotValidKeyErrorMessage);
    return nullptr;
  }

  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  std::vector<IndexedDbIndexKeys> index_keys;
  index_keys.reserve(Metadata().indexes.size());
  for (const auto& [index_id, index_metadata] : Metadata().indexes) {
    if (clone.IsEmpty()) {
      clone = serialized->Deserialize(isolate);
    }
    index_keys.push_back(GenerateIndexKeysForValue(
        isolate, Metadata().key_path, index_metadata->ToMessage(), clone));
  }

  IDBRequest* const request = IDBRequest::Create(source, transaction_.Get());
  // Chrome's IDBValueWrapper wraps a value over 64 KiB into a Blob here; the
  // Session takes values whole.
  const std::vector<uint8_t>& wire_data = serialized->wire_data();
  transaction_->Put(Id(), std::string(wire_data.begin(), wire_data.end()),
                    key ? *key : IDBKey(), put_mode, std::move(index_keys),
                    request);
  return request;
}

IDBRequest* IDBObjectStore::Delete(v8::Local<v8::Value> key,
                                   bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }
  if (transaction_->IsReadOnly()) {
    exception_state.ThrowDOMException(
        "ReadOnlyError", IDBDatabase::kTransactionReadOnlyErrorMessage);
    return nullptr;
  }

  std::optional<IndexedDbKeyRange> key_range;
  if (!IDBKeyRange::FromScriptValue(v8::Isolate::GetCurrent(), key,
                                    exception_state, &key_range)) {
    return nullptr;
  }
  if (!key_range) {
    exception_state.ThrowDOMException(
        "DataError", IDBDatabase::kNoKeyOrKeyRangeErrorMessage);
    return nullptr;
  }
  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  // deleteFunction().
  IDBRequest* const request = IDBRequest::Create(this, transaction_.Get());
  db().DeleteRange(transaction_->Id(), Id(), *std::move(key_range), request);
  return request;
}

IDBRequest* IDBObjectStore::Clear(bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }
  if (transaction_->IsReadOnly()) {
    exception_state.ThrowDOMException(
        "ReadOnlyError", IDBDatabase::kTransactionReadOnlyErrorMessage);
    return nullptr;
  }
  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  IDBRequest* const request = IDBRequest::Create(this, transaction_.Get());
  db().Clear(transaction_->Id(), Id(), request);
  return request;
}

IDBIndex* IDBObjectStore::CreateIndex(
    const std::u16string& name,
    const bindings::DOMStringOrDOMStringSequence& key_path,
    const bindings::IDBIndexParameters& options,
    bindings::ExceptionState& exception_state) {
  return CreateIndex(name, IDBKeyPathFromUnion(key_path), options,
                     exception_state);
}

IDBIndex* IDBObjectStore::CreateIndex(
    const std::u16string& name,
    const IDBKeyPath& key_path,
    const bindings::IDBIndexParameters& options,
    bindings::ExceptionState& exception_state) {
  if (!transaction_->IsVersionChange()) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        IDBDatabase::kNotVersionChangeTransactionErrorMessage);
    return nullptr;
  }
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }
  if (ContainsIndex(name)) {
    exception_state.ThrowDOMException("ConstraintError",
                                      IDBDatabase::kIndexNameTakenErrorMessage);
    return nullptr;
  }
  if (!IDBKeyPathIsValid(key_path)) {
    exception_state.ThrowDOMException(
        "SyntaxError", "The keyPath argument contains an invalid key path.");
    return nullptr;
  }
  if (key_path.type == IDBKeyPathType::kArray && options.multi_entry) {
    exception_state.ThrowDOMException(
        "InvalidAccessError",
        "The keyPath argument was an array and the multiEntry option is true.");
    return nullptr;
  }
  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  const int64_t index_id = metadata_->max_index_id + 1;
  DCHECK_NE(index_id, IDBIndexMetadata::kInvalidId);
  db().CreateIndex(transaction_->Id(), Id(), index_id, name, key_path,
                   options.unique, options.multi_entry);

  ++metadata_->max_index_id;

  scoped_refptr<IDBIndexMetadata> index_metadata =
      base::MakeRefCounted<IDBIndexMetadata>(
          name, index_id, key_path, options.unique, options.multi_entry);
  IDBIndex* const index = bindings::MakeGarbageCollected<IDBIndex>(
      index_metadata, this, transaction_.Get());
  index_names_.push_back(name);
  indexes_.push_back(index);
  metadata_->indexes.insert_or_assign(index_id, index_metadata);

  // The index's keys of the records that the store has, which the Session
  // takes before the transaction's other tasks.
  IDBRequest* const index_request =
      OpenCursor(std::nullopt, IndexedDbCursorDirection::kNext,
                 IndexedDbTaskType::kPreemptive);
  index_request->PreventPropagation();

  // The request keeps it, as Blink's request keeps its onsuccess.
  index_request->SetNativeSuccessListener(
      bindings::MakeGarbageCollected<IndexPopulator>(
          transaction_.Get(), Id(), metadata_, std::move(index_metadata)));
  return index;
}

IDBIndex* IDBObjectStore::Index(const std::u16string& name,
                                bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (transaction_->IsFinished() || transaction_->IsFinishing()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kTransactionFinishedErrorMessage);
    return nullptr;
  }

  if (IDBIndex* const index = FindCachedIndex(name)) {
    return index;
  }

  const int64_t index_id = FindIndexId(name);
  if (index_id == IDBIndexMetadata::kInvalidId) {
    exception_state.ThrowDOMException("NotFoundError",
                                      IDBDatabase::kNoSuchIndexErrorMessage);
    return nullptr;
  }

  scoped_refptr<IDBIndexMetadata> index_metadata =
      Metadata().indexes.at(index_id);
  DCHECK(index_metadata);
  IDBIndex* const index = bindings::MakeGarbageCollected<IDBIndex>(
      std::move(index_metadata), this, transaction_.Get());
  index_names_.push_back(name);
  indexes_.push_back(index);
  return index;
}

void IDBObjectStore::DeleteIndex(const std::u16string& name,
                                 bindings::ExceptionState& exception_state) {
  if (!transaction_->IsVersionChange()) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        IDBDatabase::kNotVersionChangeTransactionErrorMessage);
    return;
  }
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return;
  }
  const int64_t index_id = FindIndexId(name);
  if (index_id == IDBIndexMetadata::kInvalidId) {
    exception_state.ThrowDOMException("NotFoundError",
                                      IDBDatabase::kNoSuchIndexErrorMessage);
    return;
  }
  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return;
  }

  db().DeleteIndex(transaction_->Id(), Id(), index_id);

  metadata_->indexes.erase(index_id);
  if (IDBIndex* const index = FindCachedIndex(name)) {
    transaction_->IndexDeleted(index);
    index->MarkDeleted();
    EraseCachedIndex(name);
  }
}

IDBRequest* IDBObjectStore::OpenCursor(
    v8::Local<v8::Value> range,
    const std::u16string& direction,
    bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }

  const IndexedDbCursorDirection cursor_direction =
      IDBCursor::DirectionFromString(direction);
  std::optional<IndexedDbKeyRange> key_range;
  if (!IDBKeyRange::FromScriptValue(v8::Isolate::GetCurrent(), range,
                                    exception_state, &key_range)) {
    return nullptr;
  }

  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  return OpenCursor(std::move(key_range), cursor_direction,
                    IndexedDbTaskType::kNormal);
}

IDBRequest* IDBObjectStore::OpenCursor(std::optional<IndexedDbKeyRange> range,
                                       IndexedDbCursorDirection direction,
                                       IndexedDbTaskType task_type) {
  IDBRequest* const request = IDBRequest::Create(this, transaction_.Get());
  request->SetCursorDetails(IDBCursorType::kKeyAndValue, direction);

  db().OpenCursor(Id(), IDBIndexMetadata::kInvalidId,
                  std::move(range).value_or(IndexedDbKeyRange()), direction,
                  /*key_only=*/false, task_type, request);
  return request;
}

IDBRequest* IDBObjectStore::OpenKeyCursor(
    v8::Local<v8::Value> range,
    const std::u16string& direction,
    bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }

  const IndexedDbCursorDirection cursor_direction =
      IDBCursor::DirectionFromString(direction);
  std::optional<IndexedDbKeyRange> key_range;
  if (!IDBKeyRange::FromScriptValue(v8::Isolate::GetCurrent(), range,
                                    exception_state, &key_range)) {
    return nullptr;
  }

  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  IDBRequest* const request = IDBRequest::Create(this, transaction_.Get());
  request->SetCursorDetails(IDBCursorType::kKeyOnly, cursor_direction);

  db().OpenCursor(Id(), IDBIndexMetadata::kInvalidId,
                  std::move(key_range).value_or(IndexedDbKeyRange()),
                  cursor_direction, /*key_only=*/true,
                  IndexedDbTaskType::kNormal, request);
  return request;
}

IDBRequest* IDBObjectStore::Count(v8::Local<v8::Value> range,
                                  bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }
  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }

  std::optional<IndexedDbKeyRange> key_range;
  if (!IDBKeyRange::FromScriptValue(v8::Isolate::GetCurrent(), range,
                                    exception_state, &key_range)) {
    return nullptr;
  }

  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  IDBRequest* const request = IDBRequest::Create(this, transaction_.Get());
  db().Count(transaction_->Id(), Id(), IDBIndexMetadata::kInvalidId,
             std::move(key_range).value_or(IndexedDbKeyRange()), request);
  return request;
}

bool IDBObjectStore::IsNewlyCreated() const {
  DCHECK(transaction_->IsVersionChange());
  // Object store IDs are given in order, so one above the database's highest
  // when the upgrade began was made in it.
  return Id() > transaction_->OldMaxObjectStoreId();
}

void IDBObjectStore::MarkDeleted() {
  DCHECK(transaction_->IsVersionChange())
      << "An object store got deleted outside a versionchange transaction.";

  deleted_ = true;
  metadata_->indexes.clear();

  for (IDBIndex* const index : indexes_.ToVector()) {
    index->MarkDeleted();
  }
}

void IDBObjectStore::ClearIndexCache() {
  DCHECK(!transaction_->IsActive() || (IsDeleted() && IsNewlyCreated()));
  index_names_.clear();
  indexes_.clear();
}

void IDBObjectStore::RevertMetadata(
    scoped_refptr<IDBObjectStoreMetadata> old_metadata) {
  DCHECK(transaction_->IsVersionChange());
  DCHECK(!transaction_->IsActive());
  DCHECK(old_metadata);
  DCHECK_EQ(Id(), old_metadata->id);

  for (IDBIndex* const index : indexes_.ToVector()) {
    const int64_t index_id = index->Id();

    if (index->IsNewlyCreated(*old_metadata)) {
      // Made in this transaction, it keeps its metadata, as the spec has it.
      DCHECK(!old_metadata->indexes.contains(index_id));
      index->MarkDeleted();
      continue;
    }

    // Made before this transaction, it gets its metadata back, and is no
    // longer deleted if it was.
    const auto old_index = old_metadata->indexes.find(index_id);
    DCHECK(old_index != old_metadata->indexes.end());
    if (old_index != old_metadata->indexes.end()) {
      index->RevertMetadata(old_index->second);
    }
  }
  metadata_ = std::move(old_metadata);

  // Only an object store that was in the database when the upgrade began
  // gets its metadata back.
  deleted_ = false;
}

void IDBObjectStore::RevertDeletedIndexMetadata(IDBIndex& deleted_index) {
  DCHECK(transaction_->IsVersionChange());
  DCHECK(!transaction_->IsActive());
  DCHECK_EQ(deleted_index.ObjectStore(), this);
  DCHECK(deleted_index.IsDeleted());

  const int64_t index_id = deleted_index.Id();
  const auto old_index = metadata_->indexes.find(index_id);
  DCHECK(old_index != metadata_->indexes.end())
      << "The object store's metadata was not correctly reverted";
  if (old_index != metadata_->indexes.end()) {
    deleted_index.RevertMetadata(old_index->second);
  }
}

void IDBObjectStore::RenameIndex(int64_t index_id,
                                 const std::u16string& new_name) {
  DCHECK(transaction_->IsVersionChange());
  DCHECK(transaction_->IsActive());

  db().RenameIndex(transaction_->Id(), Id(), index_id, new_name);

  const auto metadata_iterator = metadata_->indexes.find(index_id);
  CHECK(metadata_iterator != metadata_->indexes.end()) << "Invalid index_id";
  const std::u16string old_name = metadata_iterator->second->name;

  DCHECK(FindCachedIndex(old_name))
      << "The index had to be accessed in order to be renamed.";
  DCHECK(!FindCachedIndex(new_name));
  for (std::u16string& name : index_names_) {
    if (name == old_name) {
      name = new_name;
      break;
    }
  }

  metadata_iterator->second->name = new_name;
}

int64_t IDBObjectStore::FindIndexId(const std::u16string& name) const {
  for (const auto& [id, index] : Metadata().indexes) {
    if (index->name == name) {
      DCHECK_NE(id, IDBIndexMetadata::kInvalidId);
      return id;
    }
  }
  return IDBIndexMetadata::kInvalidId;
}

IDBIndex* IDBObjectStore::FindCachedIndex(const std::u16string& name) const {
  for (size_t i = 0; i < index_names_.size(); ++i) {
    if (index_names_[i] == name) {
      return indexes_[i];
    }
  }
  return nullptr;
}

void IDBObjectStore::EraseCachedIndex(const std::u16string& name) {
  for (size_t i = 0; i < index_names_.size(); ++i) {
    if (index_names_[i] != name) {
      continue;
    }
    IDBIndex* const index = indexes_[i];
    index_names_.erase(index_names_.begin() + static_cast<ptrdiff_t>(i));
    indexes_.EraseIf([index](IDBIndex* item) { return item == index; });
    return;
  }
}

IDBRequest* IDBObjectStore::CreateGetAllRequest(
    const bindings::IDBGetAllOptions& options,
    IndexedDbGetAllResultType result_type,
    bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kObjectStoreDeletedErrorMessage);
    return nullptr;
  }

  if (!transaction_->IsActive()) {
    exception_state.ThrowDOMException("TransactionInactiveError",
                                      transaction_->InactiveErrorMessage());
    return nullptr;
  }

  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  std::optional<IndexedDbKeyRange> key_range;
  if (!IDBKeyRange::FromScriptValue(isolate,
                                    options.query.IsEmpty()
                                        ? v8::Null(isolate).As<v8::Value>()
                                        : options.query,
                                    exception_state, &key_range)) {
    return nullptr;
  }

  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return nullptr;
  }

  const uint32_t count = IDBGetAllOptionsCount(options);
  const IndexedDbCursorDirection direction =
      IDBCursor::DirectionFromString(options.direction);

  IDBRequest* const request = IDBRequest::Create(this, transaction_.Get());
  db().GetAll(transaction_->Id(), Id(), IDBIndexMetadata::kInvalidId,
              std::move(key_range).value_or(IndexedDbKeyRange()), result_type,
              count, direction, request);
  return request;
}

}  // namespace mitto
