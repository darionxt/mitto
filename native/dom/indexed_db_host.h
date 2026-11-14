// What a document's IndexedDB asks of the Session, and hears back, as Blink's
// IDBFactory, IDBDatabase, IDBTransaction and IDBCursor ask the browser over
// blink.mojom's interfaces, and hear through IDBFactoryClient and
// IDBDatabaseCallbacks. Where Mojo hands an interface over, the Page names it
// by a number the host gives: a connection and its upgrade's transaction in
// an open, a transaction in its creation, a cursor in its opening.
//
// Upstream: third_party/blink/public/mojom/indexeddb/indexeddb.mojom

#ifndef MITTO_DOM_INDEXED_DB_HOST_H_
#define MITTO_DOM_INDEXED_DB_HOST_H_

#include <stdint.h>

#include "base/functional/callback.h"
#include "mitto/indexed_db_messages.h"

namespace mitto {

class IndexedDbHost {
 public:
  using Answer = base::RepeatingCallback<void(PageIndexedDbCallback callback)>;

  // A number for a connection, a transaction or a cursor, that no other has.
  virtual uint64_t NextNumber() = 0;
  // Sends `call` with a number of the host's, and calls `answer` with what
  // answers it, each from a task of its own on the database access queue: an
  // open hears blocked and upgradeneeded before its end, and getAll() its
  // records in chunks.
  virtual void Ask(PageIndexedDbCall call, Answer answer) = 0;
  // Sends `call`, which nothing answers.
  virtual void Tell(PageIndexedDbCall call) = 0;
  // Calls `listener` with what the Session tells connection `connection`:
  // versionchange, a forced close, and the end of each of its transactions,
  // until StopListening().
  virtual void ListenToConnection(uint64_t connection, Answer listener) = 0;
  virtual void StopListening(uint64_t connection) = 0;

  // The Page has made document `document`, which takes the place of the one
  // before it: what that one asked and opened goes, as its Mojo pipes go
  // with its context, and nothing of it reaches the Session from here on.
  virtual void DocumentCommitted(uint64_t document) = 0;

 protected:
  virtual ~IndexedDbHost() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_INDEXED_DB_HOST_H_
