// What a dedicated worker and the context that made it say to each other, as
// Blink's DedicatedWorkerMessagingProxy and DedicatedWorkerObjectProxy carry
// it between the creator's thread and the worker's. The DOM's Worker and
// DedicatedWorkerGlobalScope speak through these, and the Page program, which
// runs each worker on a thread of its own, implements them.
//
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker_messaging_proxy.h
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker_object_proxy.h

#ifndef MITTO_DOM_DEDICATED_WORKER_MESSAGING_H_
#define MITTO_DOM_DEDICATED_WORKER_MESSAGING_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "mitto/dom/fetcher.h"
#include "url/gurl.h"

namespace mitto {

class MessagePortChannel;
class SerializedScriptValue;
class Worker;

// What a Worker object asks of its worker, on the creator's thread.
class WorkerMessagingProxy {
 public:
  virtual ~WorkerMessagingProxy() = default;

  // Hands `message`, with the ports it transfers, to the worker's global
  // scope once its top-level script has run, or drops it once the worker is
  // ending.
  virtual void PostMessageToWorkerGlobalScope(
      SerializedScriptValue message,
      std::vector<MessagePortChannel> ports) = 0;
  // Ends the worker, as Blink's TerminateGlobalScope() does: from now on its
  // Worker hears nothing more of it but the errors it reported before.
  virtual void Terminate() = 0;
};

// How a worker's script is to be run, from `new Worker()`'s options.
struct WorkerStartOptions {
  bool module = false;
  FetchCredentialsMode credentials = FetchCredentialsMode::kSameOrigin;
  std::u16string name;
};

// What starts a dedicated worker for a context, which the Page program gives
// each context that may start one.
class WorkerStarter {
 public:
  virtual ~WorkerStarter() = default;

  // Starts a worker of the script at `url`, which the current context may
  // read, for `worker`, which hears how it goes until Terminate().
  virtual std::unique_ptr<WorkerMessagingProxy> StartWorker(
      Worker& worker,
      const GURL& url,
      const WorkerStartOptions& options) = 0;
};

// What a dedicated worker's global scope tells its Worker object, on the
// worker's thread: each reaches the Worker from a task of the creator's.
class WorkerObjectProxy {
 public:
  virtual ~WorkerObjectProxy() = default;

  virtual void PostMessageToWorkerObject(
      SerializedScriptValue message,
      std::vector<MessagePortChannel> ports) = 0;
  // An error that the worker's scripts left uncaught and its global scope's
  // listeners did not cancel, as its error event had it.
  virtual void ReportException(std::u16string message,
                               std::u16string filename,
                               uint32_t line,
                               uint32_t column) = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_DEDICATED_WORKER_MESSAGING_H_
