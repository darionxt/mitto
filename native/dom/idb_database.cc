#include "mitto/dom/idb_database.h"

#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_idb_database.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/dom_string_list.h"
#include "mitto/dom/event.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/idb_cursor.h"
#include "mitto/dom/idb_event_dispatcher.h"
#include "mitto/dom/idb_key_conversion.h"
#include "mitto/dom/idb_object_store.h"
#include "mitto/dom/idb_request.h"
#include "mitto/dom/idb_transaction.h"
#include "mitto/dom/idb_version_change_event.h"
#include "mitto/dom/indexed_db_host.h"
#include "mitto/dom/window.h"

namespace mitto {

IDBDatabase::IDBDatabase(Window* window,
                         IndexedDbHost* host,
                         uint64_t connection)
    : window_(window), host_(host), connection_(connection) {
  host_->ListenToConnection(
      connection_, base::BindRepeating(
                       [](cppgc::WeakPersistent<IDBDatabase> database,
                          PageIndexedDbCallback callback) {
                         IDBDatabase* const target = database.Get();
                         if (!target) {
                           return;
                         }
                         RunInIDBContext(target->GetWindow(), [&] {
                           target->OnConnectionCallback(std::move(callback));
                         });
                       },
                       cppgc::WeakPersistent<IDBDatabase>(this)));
  if (window) {
    window->AddContextDestroyedCallback(base::BindOnce(
        [](cppgc::WeakPersistent<IDBDatabase> database) {
          if (database) {
            database->ContextDestroyed();
          }
        },
        cppgc::WeakPersistent<IDBDatabase>(this)));
  }
}

IDBDatabase::~IDBDatabase() = default;

const bindings::WrapperTypeInfo* IDBDatabase::GetWrapperTypeInfo() const {
  return &bindings::V8IDBDatabase::kWrapperTypeInfo;
}

void IDBDatabase::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(version_change_transaction_);
  transactions_.Trace(visitor);
}

bool IDBDatabase::HasContext() const {
  Window* const window = window_.Get();
  return window && window->HasContext();
}

void IDBDatabase::SetMetadata(const IDBDatabaseMetadata& metadata) {
  metadata_ = metadata;
}

void IDBDatabase::SetDatabaseMetadata(const IDBDatabaseMetadata& metadata) {
  metadata_.CopyFrom(metadata);
}

IDBTransaction* IDBDatabase::FindTransaction(uint64_t transaction_id) const {
  for (size_t i = 0; i < transactions_.size(); ++i) {
    if (transactions_[i]->Id() == transaction_id) {
      return transactions_[i];
    }
  }
  return nullptr;
}

void IDBDatabase::ResetCursorPrefetchCaches(uint64_t transaction_id) {
  // Blink keeps one set of every cursor; a transaction's cursors are all of
  // its connection's, and its own set holds them here.
  if (IDBTransaction* const transaction = FindTransaction(transaction_id)) {
    transaction->ResetCursorPrefetchCaches(nullptr);
  }
}

void IDBDatabase::TransactionCreated(IDBTransaction* transaction) {
  DCHECK(transaction);
  DCHECK(!FindTransaction(transaction->Id()));
  transactions_.push_back(transaction);

  if (transaction->IsVersionChange()) {
    DCHECK(!version_change_transaction_);
    version_change_transaction_ = transaction;
  }
}

void IDBDatabase::TransactionWillFinish(const IDBTransaction* transaction) {
  if (version_change_transaction_ && transaction->IsVersionChange()) {
    DCHECK_EQ(version_change_transaction_.Get(), transaction);
    version_change_transaction_ = nullptr;
  }
}

void IDBDatabase::TransactionFinished(const IDBTransaction* transaction) {
  DCHECK(transaction);
  DCHECK_EQ(FindTransaction(transaction->Id()), transaction);
  transactions_.EraseIf(
      [transaction](IDBTransaction* item) { return item == transaction; });

  TransactionWillFinish(transaction);

  if (close_pending_ && transactions_.empty()) {
    CloseConnection();
  }
}

void IDBDatabase::OnConnectionCallback(PageIndexedDbCallback callback) {
  switch (callback.op) {
    case IndexedDbCallbackOp::kForcedClose:
      ForcedClose();
      return;
    case IndexedDbCallbackOp::kVersionChange:
      VersionChange(callback.old_version, callback.new_version);
      return;
    case IndexedDbCallbackOp::kAbort:
      Abort(callback.transaction, callback.error, callback.message);
      return;
    case IndexedDbCallbackOp::kComplete:
      Complete(callback.transaction);
      return;
    default:
      // The host gives a connection nothing else.
      return;
  }
}

void IDBDatabase::ForcedClose() {
  for (IDBTransaction* const transaction : transactions_.ToVector()) {
    transaction->StartAborting(nullptr);
  }
  Close();
  DispatchEvent(MakeIDBEvent(u"close", /*bubbles=*/false,
                             /*cancelable=*/false));
}

void IDBDatabase::VersionChange(int64_t old_version, int64_t new_version) {
  if (!HasContext() || !IsConnectionOpen()) {
    return;
  }

  if (close_pending_) {
    // A transaction still runs, so versionchange does not fire, but since
    // the connection does not close at once, the Session still has the
    // other side hear blocked.
    VersionChangeIgnored();
    return;
  }

  std::optional<uint64_t> new_version_nullable;
  if (new_version != IDBDatabaseMetadata::kNoVersion) {
    new_version_nullable = static_cast<uint64_t>(new_version);
  }
  DispatchEvent(IDBVersionChangeEvent::CreateTrusted(
      u"versionchange", static_cast<uint64_t>(old_version),
      new_version_nullable));
}

void IDBDatabase::Abort(uint64_t transaction_id,
                        IndexedDbException code,
                        const std::u16string& message) {
  IDBTransaction* const transaction = FindTransaction(transaction_id);
  if (!transaction) {
    return;
  }
  transaction->OnAbort(MakeIDBException(code, message));
}

void IDBDatabase::Complete(uint64_t transaction_id) {
  IDBTransaction* const transaction = FindTransaction(transaction_id);
  if (!transaction) {
    return;
  }
  transaction->OnComplete();
}

DOMStringList* IDBDatabase::ObjectStoreNames() const {
  DOMStringList* const object_store_names =
      bindings::MakeGarbageCollected<DOMStringList>();
  for (const auto& [id, store] : metadata_.object_stores) {
    object_store_names->Append(store->name);
  }
  object_store_names->Sort();
  return object_store_names;
}

IDBObjectStore* IDBDatabase::CreateObjectStore(
    const std::u16string& name,
    const bindings::IDBObjectStoreParameters& options,
    bindings::ExceptionState& exception_state) {
  return CreateObjectStore(name, IDBKeyPathFromUnion(options.key_path),
                           options.auto_increment, exception_state);
}

IDBObjectStore* IDBDatabase::CreateObjectStore(
    const std::u16string& name,
    const IDBKeyPath& key_path,
    bool auto_increment,
    bindings::ExceptionState& exception_state) {
  if (!version_change_transaction_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      kNotVersionChangeTransactionErrorMessage);
    return nullptr;
  }
  if (!version_change_transaction_->IsActive()) {
    exception_state.ThrowDOMException(
        "TransactionInactiveError",
        version_change_transaction_->InactiveErrorMessage());
    return nullptr;
  }

  if (!key_path.IsNull() && !IDBKeyPathIsValid(key_path)) {
    exception_state.ThrowDOMException(
        "SyntaxError", "The keyPath option is not a valid key path.");
    return nullptr;
  }

  if (ContainsObjectStore(name)) {
    exception_state.ThrowDOMException("ConstraintError",
                                      kObjectStoreNameTakenErrorMessage);
    return nullptr;
  }

  if (auto_increment &&
      ((key_path.type == IDBKeyPathType::kString && key_path.string.empty()) ||
       key_path.type == IDBKeyPathType::kArray)) {
    exception_state.ThrowDOMException(
        "InvalidAccessError",
        "The autoIncrement option was set but the keyPath option was empty "
        "or an array.");
    return nullptr;
  }

  if (!IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      kDatabaseClosedErrorMessage);
    return nullptr;
  }

  const int64_t object_store_id = metadata_.max_object_store_id + 1;
  DCHECK_NE(object_store_id, IDBObjectStoreMetadata::kInvalidId);
  version_change_transaction_->CreateObjectStore(object_store_id, name,
                                                 key_path, auto_increment);

  scoped_refptr<IDBObjectStoreMetadata> store_metadata =
      base::MakeRefCounted<IDBObjectStoreMetadata>(name, object_store_id,
                                                   key_path, auto_increment);
  // The minimum index ID of Chrome's LevelDB backing store, which the
  // Session keeps too, so that both copies of the metadata agree.
  store_metadata->max_index_id = 30;
  IDBObjectStore* const object_store =
      bindings::MakeGarbageCollected<IDBObjectStore>(
          store_metadata, version_change_transaction_.Get());
  version_change_transaction_->ObjectStoreCreated(name, object_store);
  metadata_.object_stores.insert_or_assign(object_store_id,
                                           std::move(store_metadata));
  ++metadata_.max_object_store_id;

  return object_store;
}

IDBTransaction* IDBDatabase::Transaction(
    const bindings::DOMStringOrDOMStringSequence& store_names,
    const std::u16string& mode,
    const bindings::IDBTransactionOptions& options,
    bindings::ExceptionState& exception_state) {
  std::set<std::u16string> scope;
  if (const std::u16string* name = std::get_if<std::u16string>(&store_names)) {
    scope.insert(*name);
  } else {
    for (const std::u16string& item :
         std::get<std::vector<std::u16string>>(store_names)) {
      scope.insert(item);
    }
  }

  if (version_change_transaction_) {
    exception_state.ThrowDOMException(
        "InvalidStateError", "A version change transaction is running.");
    return nullptr;
  }

  if (close_pending_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      "The database connection is closing.");
    return nullptr;
  }

  if (!IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      kDatabaseClosedErrorMessage);
    return nullptr;
  }

  if (scope.empty()) {
    exception_state.ThrowDOMException("InvalidAccessError",
                                      "The storeNames parameter was empty.");
    return nullptr;
  }

  std::vector<int64_t> object_store_ids;
  for (const std::u16string& name : scope) {
    const int64_t object_store_id = FindObjectStoreId(name);
    if (object_store_id == IDBObjectStoreMetadata::kInvalidId) {
      exception_state.ThrowDOMException(
          "NotFoundError", "One of the specified object stores was not found.");
      return nullptr;
    }
    object_store_ids.push_back(object_store_id);
  }

  IndexedDbTransactionMode transaction_mode;
  if (mode == u"readonly") {
    transaction_mode = IndexedDbTransactionMode::kReadOnly;
  } else if (mode == u"readwrite") {
    transaction_mode = IndexedDbTransactionMode::kReadWrite;
  } else {
    exception_state.ThrowTypeError(
        base::StrCat({"The mode provided ('", base::UTF16ToUTF8(mode),
                      "') is not one of 'readonly' or 'readwrite'."}));
    return nullptr;
  }

  IndexedDbDurability durability = IndexedDbDurability::kDefault;
  if (options.durability == u"relaxed") {
    durability = IndexedDbDurability::kRelaxed;
  } else if (options.durability == u"strict") {
    durability = IndexedDbDurability::kStrict;
  }

  const uint64_t transaction_id = host_->NextNumber();
  CreateTransaction(transaction_id, object_store_ids, transaction_mode,
                    durability);

  return IDBTransaction::CreateNonVersionChange(
      window_.Get(), host_, transaction_id, scope, transaction_mode, durability,
      this);
}

void IDBDatabase::DeleteObjectStore(const std::u16string& name,
                                    bindings::ExceptionState& exception_state) {
  if (!version_change_transaction_) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      kNotVersionChangeTransactionErrorMessage);
    return;
  }
  if (!version_change_transaction_->IsActive()) {
    exception_state.ThrowDOMException(
        "TransactionInactiveError",
        version_change_transaction_->InactiveErrorMessage());
    return;
  }

  const int64_t object_store_id = FindObjectStoreId(name);
  if (object_store_id == IDBObjectStoreMetadata::kInvalidId) {
    exception_state.ThrowDOMException(
        "NotFoundError", "The specified object store was not found.");
    return;
  }

  if (!IsConnectionOpen()) {
    exception_state.ThrowDOMException("InvalidStateError",
                                      kDatabaseClosedErrorMessage);
    return;
  }

  version_change_transaction_->DeleteObjectStore(object_store_id);
  version_change_transaction_->ObjectStoreDeleted(object_store_id, name);
  metadata_.object_stores.erase(object_store_id);
}

void IDBDatabase::Close() {
  if (close_pending_) {
    return;
  }

  close_pending_ = true;
  UpdateKeepAlive();

  if (transactions_.empty()) {
    CloseConnection();
  }
}

void IDBDatabase::CloseConnection() {
  DCHECK(close_pending_);
  DCHECK(transactions_.empty());
  if (!connection_open_) {
    return;
  }
  connection_open_ = false;
  host_->StopListening(connection_);
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCloseConnection;
  call.connection = connection_;
  host_->Tell(std::move(call));
}

void IDBDatabase::ContextDestroyed() {
  // Closes the connection at once, without close()'s wait for transactions,
  // which would need the Session to abort them.
  for (IDBTransaction* const transaction : transactions_.ToVector()) {
    transaction->ContextDestroyed();
  }
  if (connection_open_) {
    connection_open_ = false;
    host_->StopListening(connection_);
    PageIndexedDbCall call;
    call.op = IndexedDbCallOp::kCloseConnection;
    call.connection = connection_;
    host_->Tell(std::move(call));
  }
  UpdateKeepAlive();
}

void IDBDatabase::Dispose() {
  if (!connection_open_) {
    return;
  }
  connection_open_ = false;
  // Garbage collection may not send, so the connection closes from a task.
  EventLoop::Get()
      ->TaskRunner(TaskType::kDatabaseAccess)
      ->PostTask(FROM_HERE, base::BindOnce(
                                [](IndexedDbHost* host, uint64_t connection) {
                                  host->StopListening(connection);
                                  PageIndexedDbCall call;
                                  call.op = IndexedDbCallOp::kCloseConnection;
                                  call.connection = connection;
                                  host->Tell(std::move(call));
                                },
                                host_.get(), connection_));
}

void IDBDatabase::ListenersChanged(std::u16string_view type) {
  UpdateKeepAlive();
}

void IDBDatabase::UpdateKeepAlive() {
  // Blink's HasEventListeners() counts listeners of every type; these are
  // the events that a connection hears.
  const bool has_listeners =
      HasEventListeners(u"versionchange") || HasEventListeners(u"close") ||
      HasEventListeners(u"abort") || HasEventListeners(u"error");
  if (!close_pending_ && HasContext() && has_listeners) {
    keep_alive_ = this;
  } else {
    keep_alive_.Clear();
  }
}

DispatchEventResult IDBDatabase::DispatchEventInternal(Event* event) {
  event->SetTarget(this);

  // An event that a script dispatches has no side effects.
  if (!event->IsTrusted()) {
    return EventTarget::DispatchEventInternal(event);
  }
  DCHECK(event->Type() == u"versionchange" || event->Type() == u"close");

  if (!HasContext()) {
    return DispatchEventResult::kNotCanceled;
  }

  const DispatchEventResult dispatch_result =
      EventTarget::DispatchEventInternal(event);

  if (event->Type() == u"versionchange" && !close_pending_ &&
      IsConnectionOpen()) {
    VersionChangeIgnored();
  }
  return dispatch_result;
}

int64_t IDBDatabase::FindObjectStoreId(const std::u16string& name) const {
  for (const auto& [id, store] : metadata_.object_stores) {
    if (store->name == name) {
      DCHECK_NE(id, IDBObjectStoreMetadata::kInvalidId);
      return id;
    }
  }
  return IDBObjectStoreMetadata::kInvalidId;
}

void IDBDatabase::RenameObjectStore(int64_t object_store_id,
                                    const std::u16string& new_name) {
  DCHECK(version_change_transaction_);
  DCHECK(version_change_transaction_->IsActive());
  DCHECK(metadata_.object_stores.contains(object_store_id));

  RenameObjectStore(version_change_transaction_->Id(), object_store_id,
                    new_name);

  IDBObjectStoreMetadata* const object_store_metadata =
      metadata_.object_stores.at(object_store_id).get();
  version_change_transaction_->ObjectStoreRenamed(object_store_metadata->name,
                                                  new_name);
  object_store_metadata->name = new_name;
}

void IDBDatabase::RevertObjectStoreCreation(int64_t object_store_id) {
  DCHECK(version_change_transaction_);
  DCHECK(!version_change_transaction_->IsActive());
  DCHECK(metadata_.object_stores.contains(object_store_id));
  metadata_.object_stores.erase(object_store_id);
}

void IDBDatabase::RevertObjectStoreMetadata(
    scoped_refptr<IDBObjectStoreMetadata> old_metadata) {
  DCHECK(version_change_transaction_);
  DCHECK(!version_change_transaction_->IsActive());
  DCHECK(old_metadata);
  const int64_t id = old_metadata->id;
  metadata_.object_stores.insert_or_assign(id, std::move(old_metadata));
}

void IDBDatabase::Tell(PageIndexedDbCall call) {
  if (!connection_open_) {
    return;
  }
  call.connection = connection_;
  host_->Tell(std::move(call));
}

void IDBDatabase::AskForRequest(PageIndexedDbCall call,
                                IDBRequest* request,
                                RequestAnswer on_answer) {
  if (!connection_open_) {
    return;
  }
  call.connection = connection_;
  host_->Ask(std::move(call),
             base::BindRepeating(
                 [](cppgc::WeakPersistent<IDBRequest> request,
                    RequestAnswer on_answer, PageIndexedDbCallback answer) {
                   IDBRequest* const target = request.Get();
                   if (!target) {
                     return;
                   }
                   RunInIDBContext(target->GetWindow(),
                                   [&] { (target->*on_answer)(answer); });
                 },
                 cppgc::WeakPersistent<IDBRequest>(request), on_answer));
}

void IDBDatabase::RenameObjectStore(uint64_t transaction_id,
                                    int64_t object_store_id,
                                    const std::u16string& new_name) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kRenameObjectStore;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.name = new_name;
  Tell(std::move(call));
}

void IDBDatabase::CreateTransaction(
    uint64_t transaction_id,
    const std::vector<int64_t>& object_store_ids,
    IndexedDbTransactionMode mode,
    IndexedDbDurability durability) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCreateTransaction;
  call.transaction = transaction_id;
  call.scope = object_store_ids;
  call.mode = mode;
  call.durability = durability;
  Tell(std::move(call));
}

void IDBDatabase::VersionChangeIgnored() {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kVersionChangeIgnored;
  Tell(std::move(call));
}

void IDBDatabase::Get(uint64_t transaction_id,
                      int64_t object_store_id,
                      int64_t index_id,
                      IndexedDbKeyRange key_range,
                      bool key_only,
                      IDBRequest* request) {
  ResetCursorPrefetchCaches(transaction_id);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kGet;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.index_id = index_id;
  call.key_range = std::move(key_range);
  call.key_only = key_only;
  AskForRequest(std::move(call), request, &IDBRequest::OnGet);
}

void IDBDatabase::GetAll(uint64_t transaction_id,
                         int64_t object_store_id,
                         int64_t index_id,
                         IndexedDbKeyRange key_range,
                         IndexedDbGetAllResultType result_type,
                         uint32_t max_count,
                         IndexedDbCursorDirection direction,
                         IDBRequest* request) {
  ResetCursorPrefetchCaches(transaction_id);

  if (!connection_open_) {
    return;
  }
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kGetAll;
  call.connection = connection_;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.index_id = index_id;
  call.key_range = std::move(key_range);
  call.result_type = result_type;
  call.count = max_count;
  call.direction = direction;
  host_->Ask(std::move(call),
             base::BindRepeating(
                 [](cppgc::WeakPersistent<IDBRequest> request,
                    IndexedDbGetAllResultType result_type,
                    PageIndexedDbCallback answer) {
                   IDBRequest* const target = request.Get();
                   if (!target) {
                     return;
                   }
                   RunInIDBContext(target->GetWindow(), [&] {
                     target->OnGetAll(result_type, answer);
                   });
                 },
                 cppgc::WeakPersistent<IDBRequest>(request), result_type));
}

void IDBDatabase::OpenCursor(int64_t object_store_id,
                             int64_t index_id,
                             IndexedDbKeyRange key_range,
                             IndexedDbCursorDirection direction,
                             bool key_only,
                             IndexedDbTaskType task_type,
                             IDBRequest* request) {
  IDBTransaction* const transaction = request->Transaction();
  transaction->ResetCursorPrefetchCaches(nullptr);

  if (!connection_open_) {
    return;
  }
  const uint64_t cursor = host_->NextNumber();
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kOpenCursor;
  call.connection = connection_;
  call.transaction = transaction->Id();
  call.cursor = cursor;
  call.object_store_id = object_store_id;
  call.index_id = index_id;
  call.key_range = std::move(key_range);
  call.direction = direction;
  call.key_only = key_only;
  call.task_type = task_type;
  host_->Ask(
      std::move(call),
      base::BindRepeating(
          [](IndexedDbHost* host, cppgc::WeakPersistent<IDBRequest> request,
             uint64_t cursor, PageIndexedDbCallback answer) {
            IDBRequest* const target = request.Get();
            if (target && RunInIDBContext(target->GetWindow(), [&] {
                  target->OnOpenCursor(cursor, answer);
                })) {
              return;
            }
            // The Session's cursor goes, as Blink's result does with its
            // remote when the request has gone.
            if (answer.error == IndexedDbException::kNoError && !answer.empty) {
              CloseIDBCursor(host, cursor);
            }
          },
          host_.get(), cppgc::WeakPersistent<IDBRequest>(request), cursor));
}

void IDBDatabase::Count(uint64_t transaction_id,
                        int64_t object_store_id,
                        int64_t index_id,
                        IndexedDbKeyRange key_range,
                        IDBRequest* request) {
  ResetCursorPrefetchCaches(transaction_id);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCount;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.index_id = index_id;
  call.key_range = std::move(key_range);
  AskForRequest(std::move(call), request, &IDBRequest::OnCount);
}

void IDBDatabase::Delete(uint64_t transaction_id,
                         int64_t object_store_id,
                         const IDBKey& primary_key,
                         IDBRequest* request) {
  ResetCursorPrefetchCaches(transaction_id);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kDeleteRange;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.key_range = IndexedDbKeyRange::Only(primary_key);
  AskForRequest(std::move(call), request, &IDBRequest::OnDelete);
}

void IDBDatabase::DeleteRange(uint64_t transaction_id,
                              int64_t object_store_id,
                              IndexedDbKeyRange key_range,
                              IDBRequest* request) {
  ResetCursorPrefetchCaches(transaction_id);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kDeleteRange;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.key_range = std::move(key_range);
  AskForRequest(std::move(call), request, &IDBRequest::OnDelete);
}

void IDBDatabase::GetKeyGeneratorCurrentNumber(uint64_t transaction_id,
                                               int64_t object_store_id,
                                               IDBRequest* request) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kGetKeyGeneratorCurrentNumber;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  AskForRequest(std::move(call), request,
                &IDBRequest::OnGotKeyGeneratorCurrentNumber);
}

void IDBDatabase::Clear(uint64_t transaction_id,
                        int64_t object_store_id,
                        IDBRequest* request) {
  ResetCursorPrefetchCaches(transaction_id);

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kClear;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  AskForRequest(std::move(call), request, &IDBRequest::OnClear);
}

void IDBDatabase::CreateIndex(uint64_t transaction_id,
                              int64_t object_store_id,
                              int64_t index_id,
                              const std::u16string& name,
                              const IDBKeyPath& key_path,
                              bool unique,
                              bool multi_entry) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCreateIndex;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.index.id = index_id;
  call.index.name = name;
  call.index.key_path = key_path;
  call.index.unique = unique;
  call.index.multi_entry = multi_entry;
  Tell(std::move(call));
}

void IDBDatabase::DeleteIndex(uint64_t transaction_id,
                              int64_t object_store_id,
                              int64_t index_id) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kDeleteIndex;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.index_id = index_id;
  Tell(std::move(call));
}

void IDBDatabase::RenameIndex(uint64_t transaction_id,
                              int64_t object_store_id,
                              int64_t index_id,
                              const std::u16string& new_name) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kRenameIndex;
  call.transaction = transaction_id;
  call.object_store_id = object_store_id;
  call.index_id = index_id;
  call.name = new_name;
  Tell(std::move(call));
}

void IDBDatabase::Abort(uint64_t transaction_id) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kAbort;
  call.transaction = transaction_id;
  Tell(std::move(call));
}

void IDBDatabase::DidBecomeInactive() {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kDidBecomeInactive;
  Tell(std::move(call));
}

}  // namespace mitto
