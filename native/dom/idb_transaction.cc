#include "mitto/dom/idb_transaction.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "mitto/bindings/exception_state.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/bindings/v8_idb_transaction.h"
#include "mitto/dom/dom_exception.h"
#include "mitto/dom/dom_string_list.h"
#include "mitto/dom/event.h"
#include "mitto/dom/idb_cursor.h"
#include "mitto/dom/idb_database.h"
#include "mitto/dom/idb_event_dispatcher.h"
#include "mitto/dom/idb_index.h"
#include "mitto/dom/idb_object_store.h"
#include "mitto/dom/idb_open_db_request.h"
#include "mitto/dom/idb_request.h"
#include "mitto/dom/idb_request_queue_item.h"
#include "mitto/dom/indexed_db_host.h"
#include "mitto/dom/window.h"
#include "v8/include/v8-isolate.h"

namespace mitto {
namespace {

// blink.mojom's kIDBMaxMessageSize and kIDBMaxMessageOverhead: the most that
// a put may estimate to send.
constexpr size_t kMaxPutValueSize = 134217728 - 1048576;

// The tasks of each isolate's next checkpoint. The Page program has one
// isolate, on its main thread.
std::vector<base::OnceClosure>& EndOfMicrotaskCheckpointTasks(
    v8::Isolate* isolate) {
  static base::NoDestructor<
      std::map<v8::Isolate*, std::vector<base::OnceClosure>>>
      tasks;
  const auto [found, inserted] = tasks->try_emplace(isolate);
  if (inserted) {
    isolate->AddMicrotasksCompletedCallback([](v8::Isolate* isolate, void*) {
      // EventLoop::RunEndOfMicrotaskCheckpointTasks(): those queued
      // while these run wait for the next checkpoint.
      std::vector<base::OnceClosure> due;
      due.swap(EndOfMicrotaskCheckpointTasks(isolate));
      for (base::OnceClosure& task : due) {
        std::move(task).Run();
      }
    });
  }
  return found->second;
}

// Blink's EventLoop::EnqueueEndOfMicrotaskCheckpointTask(): `task` runs once
// the next microtask checkpoint has run its microtasks, which V8 tells the
// isolate's microtasks completed callback, as it tells Blink's event loop.
void EnqueueEndOfMicrotaskCheckpointTask(v8::Isolate* isolate,
                                         base::OnceClosure task) {
  EndOfMicrotaskCheckpointTasks(isolate).push_back(std::move(task));
}

}  // namespace

// static
IDBTransaction* IDBTransaction::CreateNonVersionChange(
    Window* window,
    IndexedDbHost* host,
    uint64_t id,
    const std::set<std::u16string>& scope,
    IndexedDbTransactionMode mode,
    IndexedDbDurability durability,
    IDBDatabase* database) {
  DCHECK_NE(mode, IndexedDbTransactionMode::kVersionChange);
  DCHECK(!scope.empty());
  return bindings::MakeGarbageCollected<IDBTransaction>(
      window, host, id, scope, mode, durability, database);
}

// static
IDBTransaction* IDBTransaction::CreateVersionChange(
    Window* window,
    IndexedDbHost* host,
    uint64_t id,
    IDBDatabase* database,
    IDBOpenDBRequest* open_db_request,
    const IDBDatabaseMetadata& old_metadata) {
  return bindings::MakeGarbageCollected<IDBTransaction>(
      window, host, id, database, open_db_request, old_metadata);
}

IDBTransaction::IDBTransaction(Window* window,
                               IndexedDbHost* host,
                               uint64_t id,
                               const std::set<std::u16string>& scope,
                               IndexedDbTransactionMode mode,
                               IndexedDbDurability durability,
                               IDBDatabase* database)
    : window_(window),
      host_(host),
      id_(id),
      database_(database),
      mode_(mode),
      durability_(durability),
      scope_(scope),
      state_(kActive),
      keep_alive_(this) {
  DCHECK(database_);
  DCHECK(!scope_.empty());
  DCHECK(mode_ == IndexedDbTransactionMode::kReadOnly ||
         mode_ == IndexedDbTransactionMode::kReadWrite);

  // Inactive once the task or the event that made it has run its microtasks.
  EnqueueEndOfMicrotaskCheckpointTask(
      v8::Isolate::GetCurrent(),
      base::BindOnce(
          [](cppgc::Persistent<IDBTransaction> transaction) {
            // A transaction that finished meanwhile, as its context went,
            // has nothing to deactivate.
            if (!transaction->IsFinished()) {
              transaction->SetActive(false);
            }
          },
          cppgc::Persistent<IDBTransaction>(this)));

  database_->TransactionCreated(this);
}

IDBTransaction::IDBTransaction(Window* window,
                               IndexedDbHost* host,
                               uint64_t id,
                               IDBDatabase* database,
                               IDBOpenDBRequest* open_db_request,
                               const IDBDatabaseMetadata& old_metadata)
    : window_(window),
      host_(host),
      id_(id),
      database_(database),
      open_db_request_(open_db_request),
      mode_(IndexedDbTransactionMode::kVersionChange),
      durability_(IndexedDbDurability::kDefault),
      state_(kInactive),
      old_database_metadata_(old_metadata),
      keep_alive_(this) {
  DCHECK(database_);
  DCHECK(open_db_request_);

  database_->TransactionCreated(this);
}

IDBTransaction::~IDBTransaction() = default;

const bindings::WrapperTypeInfo* IDBTransaction::GetWrapperTypeInfo() const {
  return &bindings::V8IDBTransaction::kWrapperTypeInfo;
}

void IDBTransaction::Trace(cppgc::Visitor* visitor) const {
  EventTarget::Trace(visitor);
  visitor->Trace(window_);
  visitor->Trace(database_);
  visitor->Trace(open_db_request_);
  visitor->Trace(error_);
  request_list_.Trace(visitor);
  object_stores_.Trace(visitor);
  old_store_metadata_stores_.Trace(visitor);
  deleted_indexes_.Trace(visitor);
  cursors_.Trace(visitor);
}

bool IDBTransaction::HasContext() const {
  Window* const window = window_.Get();
  return window && window->HasContext();
}

void IDBTransaction::SetError(DOMException* error) {
  DCHECK_NE(state_, kFinished);
  DCHECK(error);

  // The first error is the true cause of the abort.
  if (!error_) {
    error_ = error;
  }
}

IDBObjectStore* IDBTransaction::FindObjectStore(
    const std::u16string& name) const {
  for (size_t i = 0; i < object_store_names_.size(); ++i) {
    if (object_store_names_[i] == name) {
      return object_stores_[i];
    }
  }
  return nullptr;
}

void IDBTransaction::EraseObjectStore(const std::u16string& name) {
  for (size_t i = 0; i < object_store_names_.size(); ++i) {
    if (object_store_names_[i] != name) {
      continue;
    }
    IDBObjectStore* const object_store = object_stores_[i];
    object_store_names_.erase(object_store_names_.begin() +
                              static_cast<ptrdiff_t>(i));
    object_stores_.EraseIf(
        [object_store](IDBObjectStore* item) { return item == object_store; });
    return;
  }
}

scoped_refptr<IDBObjectStoreMetadata> IDBTransaction::OldStoreMetadata(
    const IDBObjectStore* object_store) const {
  for (size_t i = 0; i < old_store_metadata_.size(); ++i) {
    if (old_store_metadata_stores_[i] == object_store) {
      return old_store_metadata_[i];
    }
  }
  return nullptr;
}

IDBObjectStore* IDBTransaction::ObjectStore(
    const std::u16string& name,
    bindings::ExceptionState& exception_state) {
  if (IsFinished() || IsFinishing()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kTransactionFinishedErrorMessage);
    return nullptr;
  }

  if (IDBObjectStore* const object_store = FindObjectStore(name)) {
    return object_store;
  }

  if (!IsVersionChange() && !scope_.contains(name)) {
    exception_state.ThrowDOMException(
        "NotFoundError", IDBDatabase::kNoSuchObjectStoreErrorMessage);
    return nullptr;
  }

  const int64_t object_store_id = database_->FindObjectStoreId(name);
  if (object_store_id == IDBObjectStoreMetadata::kInvalidId) {
    DCHECK(IsVersionChange());
    exception_state.ThrowDOMException(
        "NotFoundError", IDBDatabase::kNoSuchObjectStoreErrorMessage);
    return nullptr;
  }

  scoped_refptr<IDBObjectStoreMetadata> object_store_metadata =
      database_->Metadata().object_stores.at(object_store_id);
  DCHECK(object_store_metadata);

  IDBObjectStore* const object_store =
      bindings::MakeGarbageCollected<IDBObjectStore>(
          std::move(object_store_metadata), this);
  object_store_names_.push_back(name);
  object_stores_.push_back(object_store);

  if (IsVersionChange()) {
    DCHECK(!object_store->IsNewlyCreated())
        << "Object store IDs are not assigned sequentially";
    old_store_metadata_stores_.push_back(object_store);
    old_store_metadata_.push_back(object_store->Metadata().CreateCopy());
  }
  return object_store;
}

void IDBTransaction::ObjectStoreCreated(const std::u16string& name,
                                        IDBObjectStore* object_store) {
  DCHECK_NE(state_, kFinished);
  DCHECK(IsVersionChange());
  DCHECK(!FindObjectStore(name));
  DCHECK(object_store->IsNewlyCreated());
  object_store_names_.push_back(name);
  object_stores_.push_back(object_store);
}

void IDBTransaction::ObjectStoreDeleted(int64_t object_store_id,
                                        const std::u16string& name) {
  DCHECK_NE(state_, kFinished);
  DCHECK(IsVersionChange());
  IDBObjectStore* const object_store = FindObjectStore(name);
  if (!object_store) {
    // No IDBObjectStore was made for the deleted store in this transaction,
    // as when deleteObjectStore() names one that objectStore() did not give.
    // Its metadata is kept to put back if the transaction aborts, for
    // objectStoreNames.
    scoped_refptr<IDBObjectStoreMetadata> metadata =
        database_->Metadata().object_stores.at(object_store_id);
    DCHECK(metadata);
    DCHECK(metadata->name == name);
    deleted_object_stores_.push_back(std::move(metadata));
    return;
  }
  EraseObjectStore(name);
  object_store->MarkDeleted();
  if (object_store->Id() > old_database_metadata_.max_object_store_id) {
    // Made and deleted in this transaction, it stays deleted even if the
    // transaction aborts.
    DCHECK(!OldStoreMetadata(object_store));
    object_store->ClearIndexCache();
  } else {
    // Made before this transaction, its metadata was kept when objectStore()
    // gave it.
    DCHECK(OldStoreMetadata(object_store));
  }
}

void IDBTransaction::ObjectStoreRenamed(const std::u16string& old_name,
                                        const std::u16string& new_name) {
  DCHECK_NE(state_, kFinished);
  DCHECK(IsVersionChange());
  DCHECK(!FindObjectStore(new_name));
  for (std::u16string& name : object_store_names_) {
    if (name == old_name) {
      name = new_name;
      return;
    }
  }
  NOTREACHED() << "The object store had to be reached to be renamed.";
}

void IDBTransaction::IndexDeleted(IDBIndex* index) {
  DCHECK(index);
  DCHECK(!index->IsDeleted()) << "IndexDeleted called twice for the same index";

  IDBObjectStore* const object_store = index->ObjectStore();
  DCHECK_EQ(object_store->Transaction(), this);
  DCHECK(FindObjectStore(object_store->Name()))
      << "An index was deleted without accessing its object store";

  const scoped_refptr<IDBObjectStoreMetadata> old_store_metadata =
      OldStoreMetadata(object_store);
  if (!old_store_metadata) {
    // Its object store was made in this transaction, and so was the index,
    // which stays deleted if the transaction aborts: deleteIndex() needs the
    // store's IDBObjectStore, and one of a store made before keeps its
    // metadata when it is made.
    return;
  }

  if (!old_store_metadata->indexes.contains(index->Id())) {
    // The index was made, and deleted, in this transaction.
    return;
  }

  deleted_indexes_.push_back(index);
}

void IDBTransaction::RegisterCursor(IDBCursor* cursor) {
  // Those that the collector took read as null.
  cursors_.EraseIf([](IDBCursor* item) { return !item; });
  cursors_.push_back(cursor);
}

void IDBTransaction::UnregisterCursor(IDBCursor* cursor) {
  cursors_.EraseIf(
      [cursor](IDBCursor* item) { return !item || item == cursor; });
}

void IDBTransaction::ResetCursorPrefetchCaches(IDBCursor* except_cursor) {
  for (IDBCursor* const cursor : cursors_.ToVector()) {
    if (cursor && cursor != except_cursor) {
      cursor->ResetPrefetchCache();
    }
  }
}

void IDBTransaction::SetActive(bool new_is_active) {
  DCHECK_NE(state_, kFinished);
  if (IsFinishing()) {
    return;
  }

  DCHECK_NE(new_is_active, state_ == kActive);
  state_ = new_is_active ? kActive : kInactive;

  if (!new_is_active && request_list_.empty()) {
    state_ = kCommitting;
    PageIndexedDbCall call;
    call.op = IndexedDbCallOp::kCommit;
    call.transaction = id_;
    call.num_errors_handled = num_errors_handled_;
    host_->Tell(std::move(call));
  }
}

void IDBTransaction::SetActiveDuringSerialization(bool new_is_active) {
  if (new_is_active) {
    DCHECK_EQ(state_, kInactive);
    state_ = kActive;
  } else {
    DCHECK_EQ(state_, kActive);
    state_ = kInactive;
  }
}

void IDBTransaction::Abort(bindings::ExceptionState& exception_state) {
  if (IsFinishing() || IsFinished()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kTransactionFinishedErrorMessage);
    return;
  }
  StartAborting(nullptr);
}

void IDBTransaction::Commit(bindings::ExceptionState& exception_state) {
  if (IsFinishing() || IsFinished()) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kTransactionFinishedErrorMessage);
    return;
  }

  if (state_ == kInactive) {
    exception_state.ThrowDOMException(
        "InvalidStateError", IDBDatabase::kTransactionInactiveErrorMessage);
    return;
  }

  if (!HasContext()) {
    return;
  }

  state_ = kCommitting;
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCommit;
  call.transaction = id_;
  call.num_errors_handled = num_errors_handled_;
  host_->Tell(std::move(call));
}

void IDBTransaction::RegisterRequest(IDBRequest* request) {
  CHECK(request);
  CHECK_EQ(state_, kActive);
  for (size_t i = 0; i < request_list_.size(); ++i) {
    CHECK_NE(request_list_[i], request);
  }
  request_list_.push_back(request);
}

void IDBTransaction::UnregisterRequest(IDBRequest* request) {
  DCHECK(request);
  // A request that the transaction aborted is gone already.
  request_list_.EraseIf(
      [request](IDBRequest* item) { return item == request; });
}

void IDBTransaction::EnqueueResult(
    std::unique_ptr<IDBRequestQueueItem> result) {
  result_queue_.push_back(std::move(result));
  // The result can be ready at once, so it goes into the queue first.
  result_queue_.back()->StartLoading();
}

void IDBTransaction::OnResultReady() {
  // Sending a result can abort the transaction, which cancels the results
  // that wait, each of which comes back here.
  if (handling_ready_) {
    return;
  }
  const base::AutoReset<bool> reset(&handling_ready_, true);
  // Held while results are sent, which runs listeners.
  const cppgc::Persistent<IDBTransaction> self(this);

  while (!result_queue_.empty() && result_queue_.front()->IsReady()) {
    std::unique_ptr<IDBRequestQueueItem> item =
        std::move(result_queue_.front());
    result_queue_.pop_front();
    item->SendResult();
  }
}

void IDBTransaction::OnAbort(DOMException* error) {
  // Held while the abort event's listeners run.
  const cppgc::Persistent<IDBTransaction> self(this);
  if (!HasContext()) {
    Finished();
    return;
  }

  DCHECK_NE(state_, kFinished);
  if (state_ != kAborting) {
    // The Session aborted it, not the page.
    StartAborting(error, /*from_frontend=*/false);
  }

  if (IsVersionChange()) {
    database_->Close();
  }

  // The spec's abort a transaction queues these steps; the Session's abort
  // came from a task already. First the connection's upgrade transaction
  // goes, then the abort event fires, which also takes the transaction from
  // its open request, and last the transaction finishes.
  database_->TransactionWillFinish(this);
  DispatchEvent(MakeIDBEvent(u"abort", /*bubbles=*/true, /*cancelable=*/false));
  Finished();
}

void IDBTransaction::OnComplete() {
  const cppgc::Persistent<IDBTransaction> self(this);
  if (!HasContext()) {
    Finished();
    return;
  }

  DCHECK_EQ(state_, kCommitting);

  // In the order of OnAbort()'s.
  database_->TransactionWillFinish(this);
  DispatchEvent(
      MakeIDBEvent(u"complete", /*bubbles=*/false, /*cancelable=*/false));
  Finished();
}

void IDBTransaction::StartAborting(DOMException* error, bool from_frontend) {
  // The Session's aborts always come with an error.
  DCHECK(error || from_frontend);

  if (error) {
    SetError(error);
  }
  if (IsFinished() || IsFinishing()) {
    return;
  }

  state_ = kAborting;

  if (!HasContext()) {
    return;
  }

  // The spec's abort a transaction marks the object stores and indexes
  // deleted first, and aborts the requests, from tasks of their own, fifth;
  // an abort that the Session started comes from a task already.
  RevertDatabaseMetadata();
  AbortOutstandingRequests(/*queue_tasks=*/from_frontend);

  if (from_frontend && database_->IsConnectionOpen()) {
    database_->Abort(id_);
  }
}

void IDBTransaction::CreateObjectStore(int64_t object_store_id,
                                       const std::u16string& name,
                                       const IDBKeyPath& key_path,
                                       bool auto_increment) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kCreateObjectStore;
  call.transaction = id_;
  call.object_store_id = object_store_id;
  call.name = name;
  call.key_path = key_path;
  call.auto_increment = auto_increment;
  host_->Tell(std::move(call));
}

void IDBTransaction::DeleteObjectStore(int64_t object_store_id) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kDeleteObjectStore;
  call.transaction = id_;
  call.object_store_id = object_store_id;
  host_->Tell(std::move(call));
}

void IDBTransaction::Put(int64_t object_store_id,
                         std::string value,
                         IDBKey primary_key,
                         IndexedDbPutMode put_mode,
                         std::vector<IndexedDbIndexKeys> index_keys,
                         IDBRequest* request) {
  ResetCursorPrefetchCaches(nullptr);

  size_t index_keys_size = 0;
  for (const IndexedDbIndexKeys& index_key : index_keys) {
    // The index's ID.
    ++index_keys_size;
    for (const IDBKey& key : index_key.keys) {
      index_keys_size += key.SizeEstimate();
    }
  }
  const size_t estimated_size =
      value.size() + primary_key.SizeEstimate() + index_keys_size;
  if (estimated_size >= kMaxPutValueSize) {
    // Blink runs the put's callback with this at once, which only a value
    // that its wrapping in a Blob left this large reaches; here, where values
    // go whole, it comes from a task of its own, as the host's answer to a
    // call too large to send does, so that its error event does not fire
    // inside put() while the transaction is active.
    PageIndexedDbCallback answer;
    answer.op = IndexedDbCallbackOp::kPutResult;
    answer.error = IndexedDbException::kUnknownError;
    answer.message = base::UTF8ToUTF16(base::StrCat(
        {"The serialized keys and/or value are too large (size=",
         base::NumberToString(estimated_size),
         " bytes, max=", base::NumberToString(kMaxPutValueSize), " bytes)."}));
    PostIDBTask(window_.Get(), base::BindOnce(
                                   [](cppgc::WeakPersistent<IDBRequest> request,
                                      PageIndexedDbCallback answer) {
                                     if (request) {
                                       request->OnPut(answer);
                                     }
                                   },
                                   cppgc::WeakPersistent<IDBRequest>(request),
                                   std::move(answer)));
    return;
  }

  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kPut;
  call.transaction = id_;
  call.object_store_id = object_store_id;
  call.value = std::move(value);
  call.key = std::move(primary_key);
  call.put_mode = put_mode;
  call.index_keys = std::move(index_keys);
  // Values over 64 KiB go whole, where Chrome's IDBValueWrapper puts them in
  // a Blob.
  database_->AskForRequest(std::move(call), request, &IDBRequest::OnPut);
}

void IDBTransaction::SetIndexKeys(int64_t object_store_id,
                                  IDBKey primary_key,
                                  IndexedDbIndexKeys index_keys) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kSetIndexKeys;
  call.transaction = id_;
  call.object_store_id = object_store_id;
  call.primary_key = std::move(primary_key);
  call.index_keys.push_back(std::move(index_keys));
  host_->Tell(std::move(call));
}

void IDBTransaction::SetIndexReady(int64_t object_store_id) {
  PageIndexedDbCall call;
  call.op = IndexedDbCallOp::kSetIndexKeysDone;
  call.transaction = id_;
  host_->Tell(std::move(call));
}

std::u16string IDBTransaction::Mode() const {
  switch (mode_) {
    case IndexedDbTransactionMode::kReadOnly:
      return u"readonly";
    case IndexedDbTransactionMode::kReadWrite:
      return u"readwrite";
    case IndexedDbTransactionMode::kVersionChange:
      return u"versionchange";
  }
  NOTREACHED();
}

std::u16string IDBTransaction::Durability() const {
  switch (durability_) {
    case IndexedDbDurability::kDefault:
      return u"default";
    case IndexedDbDurability::kStrict:
      return u"strict";
    case IndexedDbDurability::kRelaxed:
      return u"relaxed";
  }
  NOTREACHED();
}

DOMStringList* IDBTransaction::ObjectStoreNames() const {
  if (IsVersionChange()) {
    return database_->ObjectStoreNames();
  }
  // The scope is sorted already, as Blink sorts its names.
  return bindings::MakeGarbageCollected<DOMStringList>(
      std::vector<std::u16string>(scope_.begin(), scope_.end()));
}

int64_t IDBTransaction::OldMaxObjectStoreId() const {
  DCHECK(IsVersionChange());
  return old_database_metadata_.max_object_store_id;
}

const char* IDBTransaction::InactiveErrorMessage() const {
  switch (state_) {
    case kActive:
      // Callers check IsActive() first.
      NOTREACHED();
    case kInactive:
      return IDBDatabase::kTransactionInactiveErrorMessage;
    case kCommitting:
    case kAborting:
    case kFinished:
      return IDBDatabase::kTransactionFinishedErrorMessage;
  }
  NOTREACHED();
}

void IDBTransaction::ContextDestroyed() {
  // The pending requests leave the list as they give up.
  for (IDBRequest* const request : request_list_.ToVector()) {
    request->ContextDestroyed();
  }
  keep_alive_.Clear();
}

DispatchEventResult IDBTransaction::DispatchEventInternal(Event* event) {
  event->SetTarget(this);

  // The spec's get the parent: a transaction's is its connection.
  const std::vector<EventTarget*> targets = {this, database_.Get()};

  // An event that a script dispatches has no side effects.
  if (!event->IsTrusted()) {
    return DispatchIDBEvent(*event, targets);
  }
  DCHECK(event->Type() == u"complete" || event->Type() == u"abort");

  if (!HasContext()) {
    state_ = kFinished;
    return DispatchEventResult::kNotCanceled;
  }
  DCHECK_NE(state_, kFinished);
  state_ = kFinished;

  const DispatchEventResult dispatch_result = DispatchIDBEvent(*event, targets);
  if (open_db_request_) {
    DCHECK(IsVersionChange());
    open_db_request_->TransactionDidFinishAndDispatch();
  }
  keep_alive_.Clear();
  return dispatch_result;
}

void IDBTransaction::AbortOutstandingRequests(bool queue_tasks) {
  // Held on the stack, where the collector finds them, as each abort may run
  // listeners.
  const std::vector<IDBRequest*> request_list = request_list_.ToVector();
  request_list_.clear();
  for (IDBRequest* const request : request_list) {
    request->Abort(queue_tasks);
  }
}

void IDBTransaction::RevertDatabaseMetadata() {
  DCHECK_NE(state_, kActive);
  if (!IsVersionChange()) {
    return;
  }

  // The object stores that this transaction made are deleted.
  for (IDBObjectStore* const object_store : object_stores_.ToVector()) {
    if (!object_store->IsNewlyCreated()) {
      DCHECK(OldStoreMetadata(object_store));
      continue;
    }
    DCHECK(!OldStoreMetadata(object_store));
    database_->RevertObjectStoreCreation(object_store->Id());
    object_store->MarkDeleted();
  }

  for (size_t i = 0; i < old_store_metadata_.size(); ++i) {
    IDBObjectStore* const object_store = old_store_metadata_stores_[i];
    const scoped_refptr<IDBObjectStoreMetadata>& old_metadata =
        old_store_metadata_[i];
    database_->RevertObjectStoreMetadata(old_metadata);
    object_store->RevertMetadata(old_metadata);
  }
  for (IDBIndex* const index : deleted_indexes_.ToVector()) {
    index->ObjectStore()->RevertDeletedIndexMetadata(*index);
  }
  for (scoped_refptr<IDBObjectStoreMetadata>& old_metadata :
       deleted_object_stores_) {
    database_->RevertObjectStoreMetadata(std::move(old_metadata));
  }
  deleted_object_stores_.clear();

  // Only the database's own metadata is left, as its object stores' is back.
  database_->SetDatabaseMetadata(old_database_metadata_);
}

void IDBTransaction::Finished() {
  database_->TransactionFinished(this);

  // Lets the collector take the IDBObjectStore and IDBIndex objects that no
  // script holds.
  for (IDBObjectStore* const object_store : object_stores_.ToVector()) {
    if (!IsVersionChange() || object_store->IsNewlyCreated()) {
      DCHECK(!OldStoreMetadata(object_store));
      object_store->ClearIndexCache();
    }
  }
  object_store_names_.clear();
  object_stores_.clear();

  for (IDBObjectStore* const object_store :
       old_store_metadata_stores_.ToVector()) {
    object_store->ClearIndexCache();
  }
  old_store_metadata_stores_.clear();
  old_store_metadata_.clear();
  deleted_indexes_.clear();
  deleted_object_stores_.clear();

  // Nothing fires at it any more, its context's or not.
  keep_alive_.Clear();
}

}  // namespace mitto
