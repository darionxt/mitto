#include "mitto/dom/idb_index.h"

#include <limits>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/v8_idb_index.h"
#include "mitto/dom/idb_cursor.h"
#include "mitto/dom/idb_database.h"
#include "mitto/dom/idb_get_all_options_helper.h"
#include "mitto/dom/idb_key_conversion.h"
#include "mitto/dom/idb_key_range.h"
#include "mitto/dom/idb_object_store.h"
#include "mitto/dom/idb_request.h"
#include "mitto/dom/idb_transaction.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"

namespace mitto {

IDBIndex::IDBIndex(scoped_refptr<IDBIndexMetadata> metadata,
                   IDBObjectStore* object_store,
                   IDBTransaction* transaction)
    : metadata_(std::move(metadata)),
      object_store_(object_store),
      transaction_(transaction) {
  DCHECK(object_store_);
  DCHECK(transaction_);
  DCHECK(metadata_);
  DCHECK_NE(Id(), IDBIndexMetadata::kInvalidId);
}

IDBIndex::~IDBIndex() = default;

const bindings::WrapperTypeInfo* IDBIndex::GetWrapperTypeInfo() const {
  return &bindings::V8IDBIndex::kWrapperTypeInfo;
}

void IDBIndex::Trace(cppgc::Visitor* visitor) const {
  bindings::ScriptWrappable::Trace(visitor);
  visitor->Trace(object_store_);
  visitor->Trace(transaction_);
  visitor->Trace(cached_key_path_);
}

void IDBIndex::SetName(const std::u16string& name,
                       bindings::ExceptionState& exception_state) {
  if (!transaction_->IsVersionChange()) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        IDBDatabase::kNotVersionChangeTransactionErrorMessage);
    return;
  }
  if (IsDeleted()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kIndexDeletedErrorMessage);
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
  if (object_store_->ContainsIndex(name)) {
    exception_state.ThrowDOMException("ConstraintError",
                                      IDBDatabase::kIndexNameTakenErrorMessage);
    return;
  }
  if (!db().IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kDatabaseClosedErrorMessage);
    return;
  }

  object_store_->RenameIndex(Id(), name);
}

v8::Local<v8::Value> IDBIndex::KeyPath() {
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

void IDBIndex::MarkDeleted() {
  DCHECK(transaction_->IsVersionChange())
      << "Index deleted outside versionchange transaction.";
  deleted_ = true;
}

bool IDBIndex::IsNewlyCreated(
    const IDBObjectStoreMetadata& old_object_store_metadata) const {
  DCHECK(transaction_->IsVersionChange());
  // Index IDs are given in order, so one above its object store's highest
  // when the upgrade began was made in it.
  return Id() > old_object_store_metadata.max_index_id;
}

void IDBIndex::RevertMetadata(scoped_refptr<IDBIndexMetadata> old_metadata) {
  metadata_ = std::move(old_metadata);

  // Only an index that was in the database when the upgrade began gets its
  // metadata back.
  deleted_ = false;
}

IDBRequest* IDBIndex::OpenCursor(v8::Local<v8::Value> range,
                                 const std::u16string& direction,
                                 bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kIndexDeletedErrorMessage);
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
  request->SetCursorDetails(IDBCursorType::kKeyAndValue, cursor_direction);
  db().OpenCursor(object_store_->Id(), Id(),
                  std::move(key_range).value_or(IndexedDbKeyRange()),
                  cursor_direction,
                  /*key_only=*/false, IndexedDbTaskType::kNormal, request);
  return request;
}

IDBRequest* IDBIndex::Count(v8::Local<v8::Value> range,
                            bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kIndexDeletedErrorMessage);
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
  db().Count(transaction_->Id(), object_store_->Id(), Id(),
             std::move(key_range).value_or(IndexedDbKeyRange()), request);
  return request;
}

IDBRequest* IDBIndex::OpenKeyCursor(v8::Local<v8::Value> range,
                                    const std::u16string& direction,
                                    bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kIndexDeletedErrorMessage);
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
  db().OpenCursor(object_store_->Id(), Id(),
                  std::move(key_range).value_or(IndexedDbKeyRange()),
                  cursor_direction,
                  /*key_only=*/true, IndexedDbTaskType::kNormal, request);
  return request;
}

IDBRequest* IDBIndex::Get(v8::Local<v8::Value> key,
                          bindings::ExceptionState& exception_state) {
  return GetInternal(key, exception_state, /*key_only=*/false);
}

IDBRequest* IDBIndex::GetAll(v8::Local<v8::Value> query_or_options,
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

IDBRequest* IDBIndex::GetAllKeys(v8::Local<v8::Value> query_or_options,
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

IDBRequest* IDBIndex::GetAllRecords(const bindings::IDBGetAllOptions& options,
                                    bindings::ExceptionState& exception_state) {
  return CreateGetAllRequest(options, IndexedDbGetAllResultType::kRecords,
                             exception_state);
}

IDBRequest* IDBIndex::GetKey(v8::Local<v8::Value> key,
                             bindings::ExceptionState& exception_state) {
  return GetInternal(key, exception_state, /*key_only=*/true);
}

IDBRequest* IDBIndex::GetInternal(v8::Local<v8::Value> key,
                                  bindings::ExceptionState& exception_state,
                                  bool key_only) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kIndexDeletedErrorMessage);
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
  db().Get(transaction_->Id(), object_store_->Id(), Id(), *std::move(key_range),
           key_only, request);
  return request;
}

IDBRequest* IDBIndex::CreateGetAllRequest(
    const bindings::IDBGetAllOptions& options,
    IndexedDbGetAllResultType result_type,
    bindings::ExceptionState& exception_state) {
  if (IsDeleted()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      IDBDatabase::kIndexDeletedErrorMessage);
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
  db().GetAll(transaction_->Id(), object_store_->Id(), Id(),
              std::move(key_range).value_or(IndexedDbKeyRange()), result_type,
              count, direction, request);
  return request;
}

IDBDatabase& IDBIndex::db() const {
  return transaction_->db();
}

}  // namespace mitto
