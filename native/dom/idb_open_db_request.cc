#include "mitto/dom/idb_open_db_request.h"

#include <optional>

#include "base/check.h"
#include "base/check_op.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_idb_open_db_request.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/event.h"
#include "mitto/dom/idb_any.h"
#include "mitto/dom/idb_database.h"
#include "mitto/dom/idb_event_dispatcher.h"
#include "mitto/dom/idb_factory.h"
#include "mitto/dom/idb_metadata.h"
#include "mitto/dom/idb_transaction.h"
#include "mitto/dom/idb_version_change_event.h"

namespace mitto {

IDBOpenDBRequest::IDBOpenDBRequest(Window* window,
                                   IDBFactory* factory,
                                   IndexedDbHost* host,
                                   const std::u16string& name,
                                   uint64_t connection,
                                   uint64_t transaction_id,
                                   int64_t version)
    : IDBRequest(window, nullptr, nullptr),
      factory_(factory),
      host_(host),
      db_name_(name),
      connection_(connection),
      transaction_id_(transaction_id),
      version_(version) {
  DCHECK(factory_);
}

IDBOpenDBRequest::~IDBOpenDBRequest() = default;

const bindings::WrapperTypeInfo* IDBOpenDBRequest::GetWrapperTypeInfo() const {
  return &bindings::V8IDBOpenDBRequest::kWrapperTypeInfo;
}

void IDBOpenDBRequest::Trace(cppgc::Visitor* visitor) const {
  IDBRequest::Trace(visitor);
  visitor->Trace(factory_);
}

void IDBOpenDBRequest::OnBlocked(int64_t old_version) {
  if (!CanStillSendResult()) {
    return;
  }
  std::optional<uint64_t> new_version_nullable;
  if (version_ != IDBDatabaseMetadata::kDefaultVersion) {
    new_version_nullable = static_cast<uint64_t>(version_);
  }
  DispatchEvent(IDBVersionChangeEvent::CreateTrusted(
      u"blocked", static_cast<uint64_t>(old_version), new_version_nullable));
}

bool IDBOpenDBRequest::OnUpgradeNeeded(
    int64_t old_version,
    const IDBDatabaseMetadata& metadata,
    bool data_loss,
    const std::u16string& data_loss_message) {
  if (!CanStillSendResult()) {
    return false;
  }
  DCHECK(connection_);

  IDBDatabase* const idb_database = bindings::MakeGarbageCollected<IDBDatabase>(
      GetWindow(), host_.get(), connection_);
  idb_database->SetMetadata(metadata);

  if (old_version == IDBDatabaseMetadata::kNoVersion) {
    // The database had no version before.
    old_version = IDBDatabaseMetadata::kDefaultVersion;
  }
  IDBDatabaseMetadata old_database_metadata;
  old_database_metadata.CopyFrom(metadata);
  old_database_metadata.version = old_version;

  transaction_ = IDBTransaction::CreateVersionChange(
      GetWindow(), host_.get(), transaction_id_, idb_database, this,
      old_database_metadata);
  SetResult(bindings::MakeGarbageCollected<IDBAny>(idb_database));

  if (version_ == IDBDatabaseMetadata::kNoVersion) {
    version_ = 1;
  }
  DispatchEvent(IDBVersionChangeEvent::CreateTrusted(
      u"upgradeneeded", static_cast<uint64_t>(old_version),
      static_cast<uint64_t>(version_), data_loss, data_loss_message));
  return true;
}

bool IDBOpenDBRequest::OnOpenDBSuccess(const IDBDatabaseMetadata& metadata) {
  if (!CanStillSendResult()) {
    return false;
  }

  IDBDatabase* idb_database = nullptr;
  bool made_database = false;
  if (ResultAsAny()) {
    // The upgrade made it.
    idb_database = ResultAsAny()->IdbDatabase();
    DCHECK(idb_database);
  } else {
    DCHECK(connection_);
    idb_database = bindings::MakeGarbageCollected<IDBDatabase>(
        GetWindow(), host_.get(), connection_);
    made_database = true;
    SetResult(bindings::MakeGarbageCollected<IDBAny>(idb_database));
  }
  idb_database->SetMetadata(metadata);
  DispatchEvent(
      MakeIDBEvent(u"success", /*bubbles=*/false, /*cancelable=*/false));
  return made_database;
}

void IDBOpenDBRequest::OnDeleteDBSuccess(int64_t old_version) {
  if (!CanStillSendResult()) {
    return;
  }
  // The spec has oldVersion 0 for a database that was not there.
  CHECK_GE(old_version, 0);
  SetResult(bindings::MakeGarbageCollected<IDBAny>());
  DispatchEvent(IDBVersionChangeEvent::CreateTrusted(
      u"success", static_cast<uint64_t>(old_version), std::nullopt));
}

void IDBOpenDBRequest::OnDBFactoryError(DOMException* error) {
  SendError(error);
}

bool IDBOpenDBRequest::CanStillSendResult() const {
  if (!HasContext()) {
    return false;
  }
  DCHECK(ready_state_ == kPending || ready_state_ == kDone);
  return !request_aborted_;
}

DispatchEventResult IDBOpenDBRequest::DispatchEventInternal(Event* event) {
  // An event that a script dispatches has no side effects.
  if (!event->IsTrusted()) {
    return IDBRequest::DispatchEventInternal(event);
  }
  const std::u16string type = event->Type();
  DCHECK(type == u"success" || type == u"error" || type == u"blocked" ||
         type == u"upgradeneeded");

  // A connection that closed between upgradeneeded and success fires error
  // instead.
  if (type == u"success" && ResultAsAny() &&
      ResultAsAny()->GetType() == IDBAny::kIDBDatabaseType &&
      ResultAsAny()->IdbDatabase()->IsClosePending()) {
    SetResult(nullptr);
    SendError(
        DOMException::Create(u"The connection was closed.", u"AbortError"));
    return DispatchEventResult::kNotCanceled;
  }

  return IDBRequest::DispatchEventInternal(event);
}

}  // namespace mitto
