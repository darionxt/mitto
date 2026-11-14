// A document's scripts that the parser does not run: the HTML Standard's set
// of scripts that will execute as soon as possible, and its list of scripts
// that will execute in order as soon as possible, each from a task of its own
// once it is ready, as Blink's ScriptRunner runs them. Each holds the
// document's load event back until it has run.
//
// Upstream: third_party/blink/renderer/core/script/script_runner.h
// Upstream: third_party/blink/renderer/core/script/script_runner.cc

#ifndef MITTO_DOM_DOCUMENT_SCRIPT_RUNNER_H_
#define MITTO_DOM_DOCUMENT_SCRIPT_RUNNER_H_

#include "base/time/time.h"
#include "mitto/dom/heap_vector.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class Document;
class PendingScript;

class DocumentScriptRunner final
    : public cppgc::GarbageCollected<DocumentScriptRunner> {
 public:
  explicit DocumentScriptRunner(Document& document);
  ~DocumentScriptRunner();

  void Trace(cppgc::Visitor* visitor) const;

  // Runs `pending_script`, an async or an in-order one, once it is ready.
  void QueueScriptForExecution(PendingScript* pending_script);

 private:
  // A script has its source, or has failed.
  void PendingScriptFinished(PendingScript* pending_script);
  // Runs an async script from a task, at a low priority for one that is
  // eligible, until a second has passed, as
  // PostTaskWithLowPriorityUntilTimeout() does.
  void PostAsyncScript(PendingScript* pending_script);
  void ExecutePendingScript(PendingScript* pending_script);
  // ExecutePendingScript() from a task, unless the runner has gone.
  static void RunPendingScript(
      const cppgc::WeakPersistent<DocumentScriptRunner>& runner,
      const cppgc::Persistent<PendingScript>& pending_script);

  cppgc::Member<Document> document_;
  HeapVector<PendingScript> pending_in_order_scripts_;
  HeapVector<PendingScript> pending_async_scripts_;
};

}  // namespace mitto

#endif  // MITTO_DOM_DOCUMENT_SCRIPT_RUNNER_H_
