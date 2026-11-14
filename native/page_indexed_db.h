// The library's side of a Page's IndexedDB, on the network thread: it serves
// what the Page's IDBFactory, IDBDatabase, IDBTransaction and IDBCursor ask,
// as the browser serves a renderer over blink.mojom's interfaces, from the
// Session's databases of the storage key of the Page's document, which the
// library works out itself. Where Mojo hands an interface over, the Page
// names it by a number of its own: a connection and its upgrade's
// transaction in its open, a transaction in its creation, a cursor in its
// opening.
//
// Upstream: content/browser/indexed_db/instance/bucket_context.cc
// Upstream: content/browser/indexed_db/instance/connection.cc

#ifndef MITTO_PAGE_INDEXED_DB_H_
#define MITTO_PAGE_INDEXED_DB_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/indexed_db_messages.h"

namespace mitto {

class IndexedDbConnection;
class IndexedDbContext;
class IndexedDbCursor;
class IndexedDbTransaction;

class PageIndexedDb {
 public:
  using Send = base::RepeatingCallback<void(std::string frame)>;
  using Refuse = base::RepeatingCallback<void(std::string why)>;

  PageIndexedDb(IndexedDbContext& context, Send send, Refuse refuse);
  PageIndexedDb(const PageIndexedDb&) = delete;
  PageIndexedDb& operator=(const PageIndexedDb&) = delete;
  // Closes the Page's connections, as a renderer that goes drops them.
  ~PageIndexedDb();

  // A call of the document whose storage key is `storage_key`, or nullopt if
  // its origin may not reach IndexedDB.
  void OnCall(PageIndexedDbCall call,
              const std::optional<std::string>& storage_key);

 private:
  class FactoryClient;
  class DatabaseCallbacks;

  void OnFactoryCall(PageIndexedDbCall call,
                     const std::optional<std::string>& storage_key);
  void OnDatabaseCall(PageIndexedDbCall call, IndexedDbConnection& connection);
  void OnTransactionCall(PageIndexedDbCall call);
  void OnCursorCall(PageIndexedDbCall call);

  // A connection's or an upgrade's transaction's number that the Page may
  // not use, because it does already.
  bool ConnectionInUse(uint64_t connection) const;
  IndexedDbTransaction* FindTransaction(uint64_t transaction);
  void AdoptConnection(uint64_t id,
                       std::unique_ptr<IndexedDbConnection> connection);
  void AdoptCursor(uint64_t id, std::unique_ptr<IndexedDbCursor> cursor);
  void TransactionFinished(uint64_t transaction);

  // The answer to call `call`.
  base::OnceCallback<void(PageIndexedDbCallback)> ReplyTo(uint64_t call);
  void SendReply(uint64_t call, PageIndexedDbCallback answer);
  void SendCallback(PageIndexedDbCallback callback);
  void RefuseCall(std::string why);

  const raw_ref<IndexedDbContext> context_;
  const Send send_;
  const Refuse refuse_;
  uint64_t last_call_ = 0;
  // The Page's connections, by its numbers, with those that its opens have
  // yet to give, as null.
  std::map<uint64_t, std::unique_ptr<IndexedDbConnection>> connections_;
  // The connection of each transaction that has not finished.
  std::map<uint64_t, uint64_t> transactions_;
  std::map<uint64_t, std::unique_ptr<IndexedDbCursor>> cursors_;
  base::WeakPtrFactory<PageIndexedDb> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_INDEXED_DB_H_
