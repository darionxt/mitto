// The library's side of a Page's IndexedDB, on the network thread: it serves
// what the Page's IDBFactory, IDBDatabase, IDBTransaction and IDBCursor ask,
// as the browser serves a renderer over blink.mojom's interfaces, from the
// Session's databases of the bucket that the call names, of the storage key of
// the Page's document, which the library works out itself. Where Mojo hands an
// interface over, the Page names it by a number of its own: a connection and
// its upgrade's transaction in its open, a transaction in its creation, a
// cursor in its opening.
//
// Upstream: content/browser/indexed_db/instance/bucket_context.cc
// Upstream: content/browser/indexed_db/instance/connection.cc

#ifndef MITTO_PAGE_INDEXED_DB_H_
#define MITTO_PAGE_INDEXED_DB_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "mitto/indexed_db_messages.h"

namespace mitto {

class IndexedDbBucket;
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

  // A call of the Page's context `context`. A factory's call goes to the
  // bucket that `bucket_key` names, as BucketKey() gives it, or nullopt if
  // the context's origin may not reach IndexedDB; `bucket_live` says whether
  // that bucket is still there.
  void OnCall(PageIndexedDbCall call,
              uint64_t context,
              const std::optional<std::string>& bucket_key,
              bool bucket_live = true);

  // The Page's contexts `contexts` have gone: their connections close, as a
  // renderer closes a context's when it goes.
  void ForgetContexts(const std::set<uint64_t>& contexts);

 private:
  class FactoryClient;
  class DatabaseCallbacks;

  void OnFactoryCall(PageIndexedDbCall call,
                     uint64_t context,
                     const std::optional<std::string>& bucket_key,
                     bool bucket_live);
  // A factory's call of a bucket that has been deleted, which Chrome's
  // MissingBucketErrorEndpoint answers.
  void FailFactoryCall(PageIndexedDbCall call);
  void RunFactoryCall(PageIndexedDbCall call, IndexedDbBucket& bucket);
  void OnDatabaseCall(PageIndexedDbCall call, IndexedDbConnection& connection);
  void OnTransactionCall(PageIndexedDbCall call);
  void OnCursorCall(PageIndexedDbCall call);

  // A connection's or an upgrade's transaction's number that the Page may
  // not use, because it does already.
  bool ConnectionInUse(uint64_t connection) const;
  IndexedDbTransaction* FindTransaction(uint64_t transaction);
  // Takes the connection that an open gives, unless the context that asked
  // has gone, and returns whether it took it.
  bool AdoptConnection(uint64_t id,
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
  // The context that opened each connection.
  std::map<uint64_t, uint64_t> connection_contexts_;
  // The connection of each transaction that has not finished.
  std::map<uint64_t, uint64_t> transactions_;
  std::map<uint64_t, std::unique_ptr<IndexedDbCursor>> cursors_;
  // The getAll() calls that a chunk too large for a frame ended, whose other
  // chunks go unsent.
  std::set<uint64_t> failed_get_alls_;
  base::WeakPtrFactory<PageIndexedDb> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_INDEXED_DB_H_
