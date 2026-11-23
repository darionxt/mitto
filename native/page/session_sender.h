// What the threads of a Page send to its Session: the main thread and each
// dedicated worker's thread. The I/O thread sends frames in the order they
// reach it, and the Session holds the numbers of requests and the IDs of
// contexts to be larger than those before them, so this takes each such
// number under one lock with the sending of the frame that carries it, and two
// threads' frames reach the Session in the order of their numbers, the
// questions of the Cache API and of Storage Buckets and the calls of IndexedDB
// among them. It also keeps where the response of each request of a worker
// goes, and where each answer of those goes, for the I/O thread to hand it to
// the thread of the context that asked, and which contexts the Session has
// forgotten: under the same lock, nothing that such a context sends goes after
// the frame that made the Session forget it, as Chrome's browser drops what
// the pipes of a context that went still carry.

#ifndef MITTO_PAGE_SESSION_SENDER_H_
#define MITTO_PAGE_SESSION_SENDER_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "mitto/cache_storage_messages.h"
#include "mitto/indexed_db_messages.h"
#include "mitto/page_messages.h"
#include "mitto/storage_bucket_messages.h"

namespace mitto {

class SessionSender final : public base::RefCountedThreadSafe<SessionSender> {
 public:
  // Takes a frame of the response of a request, a response started, part of
  // a body or the end, on the I/O thread.
  using ResponseRoute = base::RepeatingCallback<void(FrameFromSession frame)>;
  // Takes an answer of the Cache API to the host numbered `host`, on the I/O
  // thread, for the host's thread.
  using CacheStorageRoute =
      base::RepeatingCallback<void(uint64_t host,
                                   PageCacheStorageAnswer answer)>;
  // Takes what IndexedDB's Session sends the host numbered `host`, on the I/O
  // thread, for the host's thread.
  using IndexedDbRoute =
      base::RepeatingCallback<void(uint64_t host,
                                   PageIndexedDbCallback callback)>;
  // Takes a Storage Buckets answer to the host numbered `host`, on the I/O
  // thread, for the host's thread.
  using StorageBucketRoute =
      base::RepeatingCallback<void(uint64_t host,
                                   PageStorageBucketAnswer answer)>;

  // `send` hands a frame to the I/O thread, from any thread.
  explicit SessionSender(base::RepeatingCallback<void(std::string)> send);

  SessionSender(const SessionSender&) = delete;
  SessionSender& operator=(const SessionSender&) = delete;

  // Sends `frame`, which names no context. Any thread.
  void Send(std::string frame);

  // Sends `frame`, which `context` sends, unless the Session has forgotten
  // `context`. Any thread.
  void SendFromContext(uint64_t context, std::string frame);

  // Takes the number of the next request, of `context`, sends the frame that
  // `encode` makes of it, and returns it. Without a route, its response goes
  // to the main thread; with one, as for a worker's request, to `route` until
  // its end. A request of a context that the Session has forgotten takes its
  // number and goes nowhere. Any thread.
  uint64_t SendRequest(uint64_t context,
                       base::FunctionRef<std::string(uint64_t number)> encode,
                       ResponseRoute route = ResponseRoute());

  // Sends `frame`, a cancel of request `number`, credit for its body or part
  // of the body it sends, unless the request went nowhere. Any thread.
  void SendForRequest(uint64_t number, std::string frame);

  // Takes the ID of the next context, a top-level document, sends the frame
  // that `encode` makes of it, and returns it. It takes the place of every
  // context before it, which the Session forgets. Any thread.
  uint64_t SendTopLevelDocument(
      base::FunctionRef<std::string(uint64_t id)> encode);

  // Takes the ID of the next context, a worker that `parent` started, sends
  // the frame that `encode` makes of it, and returns it. One under a context
  // that the Session has forgotten goes unannounced, and is forgotten too.
  // Any thread.
  uint64_t SendContext(uint64_t parent,
                       base::FunctionRef<std::string(uint64_t id)> encode);

  // Takes the ID of the next context, a frame's document under `parent` that
  // takes the place of the frame's document `replaced`, sends the frame that
  // `encode` makes of it, and returns it. The Session forgets `replaced` and
  // the contexts under it, and so does this. One under a context that the
  // Session has forgotten goes unannounced, and is forgotten too. Any
  // thread.
  uint64_t SendFrameDocument(
      uint64_t parent,
      uint64_t replaced,
      base::FunctionRef<std::string(uint64_t id)> encode);

  // Says that `context` has gone, unless the Session has forgotten it, and
  // forgets it and the contexts under it, which the Session forgets with it.
  // Any thread.
  void SendContextGone(uint64_t context);

  // Hands `frame`, a frame of a response or credit for a request body, to the
  // route of its request, and forgets the route once the request ends.
  // Returns false for a request that has none, whose response goes to the
  // main thread. The I/O thread.
  bool Route(FrameFromSession& frame);

  // The Cache API of `context`, whose answers go to `route`: its number. Any
  // thread, as are the others below but the last.
  uint64_t AddCacheStorageHost(uint64_t context, CacheStorageRoute route);
  // The host has gone, or its document has: what it asked, held or opened
  // goes unheard.
  void RemoveCacheStorageHost(uint64_t host);
  // Takes the number of the Page's next Cache API question, of `host`, sends
  // the frame that `encode` makes of it, and returns it. A question of a
  // context that the Session has forgotten takes its number and goes nowhere,
  // and no answer comes.
  uint64_t SendCacheStorageQuestion(
      uint64_t host,
      base::FunctionRef<std::string(uint64_t question)> encode);
  // The number of a put body of `host`, whose requests for it go to the host
  // until DropCacheStoragePutBody().
  uint64_t AddCacheStoragePutBody(uint64_t host);
  void DropCacheStoragePutBody(uint64_t body);
  // A stored body that `host` reads, whose parts go to the host until it ends
  // or DropCacheStorageBody().
  void AddCacheStorageBody(uint64_t host, uint64_t body);
  void DropCacheStorageBody(uint64_t body);
  // Hands `answer` to the route of its host. One for a host that has gone
  // goes unheard, and one for a question or put body that the Page never
  // numbered is a refusal, which this returns. The I/O thread.
  std::optional<std::string> RouteCacheStorage(PageCacheStorageAnswer answer);

  // The IndexedDB of `context`, whose callbacks go to `route`: its number.
  // Any thread, as are the others below but the last.
  uint64_t AddIndexedDbHost(uint64_t context, IndexedDbRoute route);
  // The host has gone, or its document has: what it asked and opened goes
  // unheard.
  void RemoveIndexedDbHost(uint64_t host);
  // A number for a connection, a transaction or a cursor that no other of the
  // Page's has.
  uint64_t NextIndexedDbNumber();
  // Takes a number as the Page's next IndexedDB call, of `host`, sends the
  // frame that `encode` makes of it, and returns it. What answers it goes to
  // the host until ForgetIndexedDbCall(). A call of a context that the
  // Session has forgotten takes its number and goes nowhere, and nothing
  // answers it.
  uint64_t SendIndexedDbCall(
      uint64_t host,
      base::FunctionRef<std::string(uint64_t call)> encode);
  void ForgetIndexedDbCall(uint64_t call);
  // What the Session tells connection `connection` goes to `host` until
  // ForgetIndexedDbConnection().
  void AddIndexedDbConnection(uint64_t host, uint64_t connection);
  void ForgetIndexedDbConnection(uint64_t connection);
  // Hands `callback` to the route of its host. One for a host that has gone
  // goes unheard, and an answer to a call that the Page never numbered is a
  // refusal, which this returns. The I/O thread.
  std::optional<std::string> RouteIndexedDb(PageIndexedDbCallback callback);

  // The Storage Buckets API of `context`, whose answers go to `route`: its
  // number. Any thread, as are the others below but the last.
  uint64_t AddStorageBucketHost(uint64_t context, StorageBucketRoute route);
  // The host has gone, or its document has: what it asked goes unheard.
  void RemoveStorageBucketHost(uint64_t host);
  // Takes the number of the Page's next Storage Buckets question, of `host`,
  // sends the frame that `encode` makes of it, and returns it. A question of a
  // context that the Session has forgotten takes its number and goes nowhere,
  // and no answer comes.
  uint64_t SendStorageBucketQuestion(
      uint64_t host,
      base::FunctionRef<std::string(uint64_t question)> encode);
  // Hands `answer` to the route of its host. One for a host that has gone
  // goes unheard, and one for a question that the Page never numbered is a
  // refusal, which this returns. The I/O thread.
  std::optional<std::string> RouteStorageBucket(PageStorageBucketAnswer answer);

 private:
  friend class base::RefCountedThreadSafe<SessionSender>;
  ~SessionSender();

  // Forgets `context` and the contexts under it.
  void ForgetUnder(uint64_t context) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  const base::RepeatingCallback<void(std::string)> send_;
  base::Lock lock_;
  uint64_t last_request_ GUARDED_BY(lock_) = 0;
  uint64_t last_context_ GUARDED_BY(lock_) = kNoDocument;
  std::map<uint64_t, ResponseRoute> routes_ GUARDED_BY(lock_);
  // The parent of each context that the Session knows, kNoDocument for the
  // top-level document. Each comes after its parent.
  std::map<uint64_t, uint64_t> parents_ GUARDED_BY(lock_);
  // The contexts that the Session has forgotten, and the requests of theirs
  // that went nowhere.
  std::set<uint64_t> forgotten_ GUARDED_BY(lock_);
  std::set<uint64_t> unsent_requests_ GUARDED_BY(lock_);
  // A host of a context's, whose answers go to `route`.
  template <typename Route>
  struct Host {
    uint64_t context;
    Route route;
  };

  // Whether `host` is one of a context that the Session has forgotten.
  template <typename Route>
  bool OfForgotten(const Host<Route>* host) const
      EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    return host && forgotten_.contains(host->context);
  }

  uint64_t last_cache_storage_host_ GUARDED_BY(lock_) = 0;
  std::map<uint64_t, Host<CacheStorageRoute>> cache_storage_hosts_
      GUARDED_BY(lock_);
  uint64_t last_cache_storage_question_ GUARDED_BY(lock_) = 0;
  uint64_t last_cache_storage_put_body_ GUARDED_BY(lock_) = 0;
  // The host of each question that waits for its answer, of each put body
  // that the Session may ask for, and of each stored body being read.
  std::map<uint64_t, uint64_t> cache_storage_questions_ GUARDED_BY(lock_);
  std::map<uint64_t, uint64_t> cache_storage_put_bodies_ GUARDED_BY(lock_);
  std::map<uint64_t, uint64_t> cache_storage_bodies_ GUARDED_BY(lock_);
  uint64_t last_indexed_db_host_ GUARDED_BY(lock_) = 0;
  std::map<uint64_t, Host<IndexedDbRoute>> indexed_db_hosts_ GUARDED_BY(lock_);
  // The last of the numbers that calls, connections, transactions and cursors
  // share.
  uint64_t last_indexed_db_number_ GUARDED_BY(lock_) = 0;
  // The host of each call that waits for what answers it, and of each
  // connection that hears what the Session tells it.
  std::map<uint64_t, uint64_t> indexed_db_calls_ GUARDED_BY(lock_);
  std::map<uint64_t, uint64_t> indexed_db_connections_ GUARDED_BY(lock_);
  uint64_t last_storage_bucket_host_ GUARDED_BY(lock_) = 0;
  std::map<uint64_t, Host<StorageBucketRoute>> storage_bucket_hosts_
      GUARDED_BY(lock_);
  uint64_t last_storage_bucket_question_ GUARDED_BY(lock_) = 0;
  // The host of each question that waits for its answer.
  std::map<uint64_t, uint64_t> storage_bucket_questions_ GUARDED_BY(lock_);
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_SENDER_H_
