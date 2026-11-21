// The IndexedDB of a context through the Page's Session: of the Page's
// documents on the main thread, one after another, or of a dedicated worker on
// its thread. Each call goes over the channel with a number that the
// SessionSender takes for the whole Page, as it does those of connections,
// transactions and cursors, and what answers it, or what the Session tells a
// connection, comes back as a task of its own, on the context's database
// access queue, which Blink binds IndexedDB's interfaces to.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_factory.cc

#ifndef MITTO_PAGE_SESSION_INDEXED_DB_H_
#define MITTO_PAGE_SESSION_INDEXED_DB_H_

#include <stdint.h>

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/indexed_db_host.h"
#include "mitto/page/session_sender.h"
#include "mitto/page_messages.h"

namespace mitto {

class SessionIndexedDb final : public IndexedDbHost {
 public:
  SessionIndexedDb();
  SessionIndexedDb(const SessionIndexedDb&) = delete;
  SessionIndexedDb& operator=(const SessionIndexedDb&) = delete;
  ~SessionIndexedDb() override;

  // On the context's thread, before the first call: what sends its frames
  // and routes what answers them, the queue that brings it, and how the Page
  // refuses what the Session sent.
  void SetSender(scoped_refptr<SessionSender> sender,
                 scoped_refptr<base::SingleThreadTaskRunner> runner,
                 base::RepeatingCallback<void(std::string why)> refuse);

  // IndexedDbHost, on the context's thread. A worker's commits once, as its
  // ID:
  uint64_t NextNumber() override;
  void Ask(PageIndexedDbCall call, Answer answer) override;
  void Tell(PageIndexedDbCall call) override;
  void ListenToConnection(uint64_t connection, Answer listener) override;
  void StopListening(uint64_t connection) override;
  void DocumentCommitted(uint64_t document) override;

  // From a task of its own: what the Session sent the document or worker
  // that `host` stands for, which goes unheard once another has its place.
  void OnCallback(uint64_t host, PageIndexedDbCallback callback);

 private:
  void Send(const PageIndexedDbCall& call);

  // Whether `call` names a connection, a transaction or a cursor of a
  // document before, which has gone with it.
  bool NamesWhatWent(const PageIndexedDbCall& call) const;

  scoped_refptr<SessionSender> sender_;
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  base::RepeatingCallback<void(std::string)> refuse_;
  // The document or worker, which every call names, and the sender's number
  // of its host.
  uint64_t document_ = kNoDocument;
  uint64_t host_ = 0;
  // A number the Page took when the document committed: what names the
  // connections, transactions and cursors of the documents before, whose
  // numbers are smaller, goes unsent.
  uint64_t first_number_ = 0;
  std::map<uint64_t, Answer> calls_;
  std::map<uint64_t, Answer> listeners_;
  // The connection of each open that waits for its end, whose route the
  // Session's callbacks take from when the open goes.
  std::map<uint64_t, uint64_t> opens_;
  // The connection of each transaction, which an abort names.
  std::map<uint64_t, uint64_t> transaction_connections_;
  base::WeakPtrFactory<SessionIndexedDb> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_INDEXED_DB_H_
