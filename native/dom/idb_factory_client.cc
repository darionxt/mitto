#include "mitto/dom/idb_factory_client.h"

#include <utility>

#include "mitto/dom/idb_event_dispatcher.h"
#include "mitto/dom/idb_metadata.h"
#include "mitto/dom/idb_open_db_request.h"
#include "mitto/dom/idb_request.h"
#include "mitto/dom/indexed_db_host.h"

namespace mitto {

IDBFactoryClient::IDBFactoryClient(IDBOpenDBRequest* request,
                                   IndexedDbHost* host,
                                   uint64_t connection)
    : request_(request), host_(host), connection_(connection) {}

IDBFactoryClient::~IDBFactoryClient() = default;

void IDBFactoryClient::OnAnswer(PageIndexedDbCallback answer) {
  const bool gives_connection =
      connection_ != 0 && !connection_given_ &&
      (answer.op == IndexedDbCallbackOp::kUpgradeNeeded ||
       answer.op == IndexedDbCallbackOp::kOpenSuccess);
  if (gives_connection) {
    connection_given_ = true;
  }
  const bool taken = Deliver(answer);
  if (gives_connection && !taken) {
    // Nothing holds the connection, which closes, as Blink drops the remote
    // of a database that no IDBDatabase took.
    PageIndexedDbCall call;
    call.op = IndexedDbCallOp::kCloseConnection;
    call.connection = connection_;
    host_->Tell(std::move(call));
  }
}

bool IDBFactoryClient::Deliver(const PageIndexedDbCallback& answer) {
  // Held while listeners run, after Detach() too.
  const cppgc::Persistent<IDBOpenDBRequest> request(request_.Get());
  if (!request) {
    return false;
  }
  bool taken = false;
  const bool ran = RunInIDBContext(request->GetWindow(), [&] {
    switch (answer.op) {
      case IndexedDbCallbackOp::kError:
        Detach();
        // When the Session clears a transaction's tasks, the task of this
        // call may go with them, which needs no answer.
        if (answer.error != IndexedDbException::kIgnorableAbortError) {
          request->OnDBFactoryError(
              MakeIDBException(answer.error, answer.message));
        }
        return;
      case IndexedDbCallbackOp::kBlocked:
        // More answers come: upgradeneeded, success or an error.
        request->OnBlocked(answer.old_version);
        return;
      case IndexedDbCallbackOp::kUpgradeNeeded:
        // Success or an error follows.
        taken = request->OnUpgradeNeeded(
            answer.old_version,
            IDBDatabaseMetadata::FromMessage(answer.metadata), answer.data_loss,
            answer.message);
        return;
      case IndexedDbCallbackOp::kOpenSuccess:
        Detach();
        taken = request->OnOpenDBSuccess(
            IDBDatabaseMetadata::FromMessage(answer.metadata));
        return;
      case IndexedDbCallbackOp::kDeleteSuccess:
        Detach();
        request->OnDeleteDBSuccess(answer.old_version);
        return;
      default:
        // The host gives an open or a delete nothing else.
        return;
    }
  });
  if (!ran) {
    // The window's context went, which detaches Blink's client.
    Detach();
  }
  return taken;
}

}  // namespace mitto
