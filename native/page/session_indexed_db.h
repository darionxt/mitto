// The IndexedDB of a Page's documents through its Session, on the Page
// program's main thread: each call goes over the channel, and what answers
// it, or what the Session tells a connection, comes back as a task of its
// own, on the database access queue that Blink binds IndexedDB's interfaces
// to.
//
// Upstream: third_party/blink/renderer/modules/indexeddb/idb_factory.cc

#ifndef MITTO_PAGE_SESSION_INDEXED_DB_H_
#define MITTO_PAGE_SESSION_INDEXED_DB_H_

#include <stdint.h>

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "mitto/dom/indexed_db_host.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page_messages.h"

namespace mitto {

class SessionIndexedDb final : public IndexedDbHost {
 public:
  SessionIndexedDb();
  SessionIndexedDb(const SessionIndexedDb&) = delete;
  SessionIndexedDb& operator=(const SessionIndexedDb&) = delete;
  ~SessionIndexedDb() override;

  // On the main thread, before the first call: how frames go to the
  // Session, and how the Page refuses what it sent.
  void SetSend(SendFrame send,
               base::RepeatingCallback<void(std::string why)> refuse);

  // IndexedDbHost, on the main thread:
  uint64_t NextNumber() override;
  void Ask(PageIndexedDbCall call, Answer answer) override;
  void Tell(PageIndexedDbCall call) override;
  void ListenToConnection(uint64_t connection, Answer listener) override;
  void StopListening(uint64_t connection) override;
  void DocumentCommitted(uint64_t document) override;

  // On the main thread, from a task of its own: what the Session sent.
  void OnCallback(PageIndexedDbCallback callback);

 private:
  void Send(const PageIndexedDbCall& call);

  SendFrame send_;
  base::RepeatingCallback<void(std::string)> refuse_;
  uint64_t last_number_ = 0;
  // The Page's document, which every call names.
  uint64_t document_ = kNoDocument;
  // The last number of the documents before: their calls' answers may still
  // be on their way, and go unheard, and what names their connections,
  // transactions and cursors goes unsent.
  uint64_t last_number_before_ = 0;
  std::map<uint64_t, Answer> calls_;
  std::map<uint64_t, Answer> listeners_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_INDEXED_DB_H_
