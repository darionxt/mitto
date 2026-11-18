// What the threads of a Page send to its Session: the main thread and each
// dedicated worker's thread. The I/O thread sends frames in the order they
// reach it, and the Session holds the numbers of requests and the IDs of
// contexts to be larger than those before them, so this takes each such
// number under one lock with the sending of the frame that carries it, and two
// threads' frames reach the Session in the order of their numbers, the
// questions of the Cache API and the calls of IndexedDB among them. It also
// keeps where the response of each request of a worker goes, and where each
// answer of the Cache API and of IndexedDB goes, for the I/O thread to hand it
// to the thread of the context that asked.

#ifndef MITTO_PAGE_SESSION_SENDER_H_
#define MITTO_PAGE_SESSION_SENDER_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "mitto/cache_storage_messages.h"
#include "mitto/indexed_db_messages.h"
#include "mitto/page_messages.h"

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

  // `send` hands a frame to the I/O thread, from any thread.
  explicit SessionSender(base::RepeatingCallback<void(std::string)> send);

  SessionSender(const SessionSender&) = delete;
  SessionSender& operator=(const SessionSender&) = delete;

  // Sends `frame`. Any thread.
  void Send(std::string frame);

  // Takes the number of the next request, sends the frame that `encode` makes
  // of it, and returns it. Without a route, its response goes to the main
  // thread; with one, as for a worker's request, to `route` until its end.
  // Any thread.
  uint64_t SendRequest(base::FunctionRef<std::string(uint64_t number)> encode,
                       ResponseRoute route = ResponseRoute());

  // Takes the ID of the next context, a document or a worker, sends the frame
  // that `encode` makes of it, and returns it. Any thread.
  uint64_t SendContext(base::FunctionRef<std::string(uint64_t id)> encode);

  // Hands `frame`, a frame of a response, to the route of its request, and
  // forgets the route once the request ends. Returns false for a request that
  // has none, whose response goes to the main thread. The I/O thread.
  bool Route(FrameFromSession& frame);

  // The Cache API of a context, whose answers go to `route`: its number. Any
  // thread, as are the others below but the last.
  uint64_t AddCacheStorageHost(CacheStorageRoute route);
  // The host has gone, or its document has: what it asked, held or opened
  // goes unheard.
  void RemoveCacheStorageHost(uint64_t host);
  // Takes the number of the Page's next Cache API question, of `host`, sends
  // the frame that `encode` makes of it, and returns it.
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

  // The IndexedDB of a context, whose callbacks go to `route`: its number.
  // Any thread, as are the others below but the last.
  uint64_t AddIndexedDbHost(IndexedDbRoute route);
  // The host has gone, or its document has: what it asked and opened goes
  // unheard.
  void RemoveIndexedDbHost(uint64_t host);
  // A number for a connection, a transaction or a cursor that no other of the
  // Page's has.
  uint64_t NextIndexedDbNumber();
  // Takes a number as the Page's next IndexedDB call, of `host`, sends the
  // frame that `encode` makes of it, and returns it. What answers it goes to
  // the host until ForgetIndexedDbCall().
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

 private:
  friend class base::RefCountedThreadSafe<SessionSender>;
  ~SessionSender();

  const base::RepeatingCallback<void(std::string)> send_;
  base::Lock lock_;
  uint64_t last_request_ GUARDED_BY(lock_) = 0;
  uint64_t last_context_ GUARDED_BY(lock_) = kNoDocument;
  std::map<uint64_t, ResponseRoute> routes_ GUARDED_BY(lock_);
  uint64_t last_cache_storage_host_ GUARDED_BY(lock_) = 0;
  std::map<uint64_t, CacheStorageRoute> cache_storage_hosts_ GUARDED_BY(lock_);
  uint64_t last_cache_storage_question_ GUARDED_BY(lock_) = 0;
  uint64_t last_cache_storage_put_body_ GUARDED_BY(lock_) = 0;
  // The host of each question that waits for its answer, of each put body
  // that the Session may ask for, and of each stored body being read.
  std::map<uint64_t, uint64_t> cache_storage_questions_ GUARDED_BY(lock_);
  std::map<uint64_t, uint64_t> cache_storage_put_bodies_ GUARDED_BY(lock_);
  std::map<uint64_t, uint64_t> cache_storage_bodies_ GUARDED_BY(lock_);
  uint64_t last_indexed_db_host_ GUARDED_BY(lock_) = 0;
  std::map<uint64_t, IndexedDbRoute> indexed_db_hosts_ GUARDED_BY(lock_);
  // The last of the numbers that calls, connections, transactions and cursors
  // share.
  uint64_t last_indexed_db_number_ GUARDED_BY(lock_) = 0;
  // The host of each call that waits for what answers it, and of each
  // connection that hears what the Session tells it.
  std::map<uint64_t, uint64_t> indexed_db_calls_ GUARDED_BY(lock_);
  std::map<uint64_t, uint64_t> indexed_db_connections_ GUARDED_BY(lock_);
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_SENDER_H_
