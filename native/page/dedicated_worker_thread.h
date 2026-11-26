// A dedicated worker's thread, as Blink's DedicatedWorkerThread and
// WorkerBackingThread run one: a thread of its own in the Page process, with
// its scheduler, its isolate, which V8Initializer::InitializeWorker() sets up,
// and the context of its DedicatedWorkerGlobalScope, made as soon as the
// thread starts. The creator's thread makes it once the worker's script has
// started to come, hands it the script and the messages for the worker, and
// ends it. The worker's thread tells its creator what happens through its
// object proxy, from which each news reaches the creator in a task of its own.
//
// Upstream: third_party/blink/renderer/core/workers/worker_thread.cc
// Upstream: third_party/blink/renderer/core/workers/dedicated_worker_thread.cc
// Upstream: third_party/blink/renderer/core/workers/worker_backing_thread.cc
// Upstream: third_party/blink/renderer/core/workers/worker_global_scope.cc
// Upstream: third_party/blink/renderer/core/workers/worker_classic_script_loader.cc

#ifndef MITTO_PAGE_DEDICATED_WORKER_THREAD_H_
#define MITTO_PAGE_DEDICATED_WORKER_THREAD_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "base/threading/platform_thread.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/dom/dedicated_worker_messaging.h"
#include "mitto/dom/fetcher.h"
#include "mitto/page/session_sender.h"
#include "mitto/page_messages.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace v8 {
class Isolate;
}  // namespace v8

namespace mitto {

class MessagePortChannel;
class SerializedScriptValue;
class WorkerScheduler;

// What a worker's thread tells its creator beyond what its global scope does,
// from the worker's thread.
class DedicatedWorkerObjectProxy : public WorkerObjectProxy {
 public:
  // The top-level script has run, or thrown: messages may go to the worker.
  virtual void DidEvaluateTopLevelScript() = 0;
  // The script was not one to run, as a response that is not 2xx is not.
  virtual void DidFailToFetchScript() = 0;
  // The worker called close(), or its script did not come: it ends.
  virtual void DidCloseWorkerGlobalScope() = 0;
  // The worker's thread has let go of everything, and is about to end.
  virtual void DidTerminateWorkerThread() = 0;
};

class DedicatedWorkerThread final : public base::PlatformThread::Delegate {
 public:
  // What the worker starts with, from its creator.
  struct Params {
    Params();
    Params(Params&&);
    Params& operator=(Params&&);
    ~Params();

    // The worker's ID, which its messages to the Session carry.
    uint64_t id = kNoDocument;
    // Its origin, its creator's.
    url::Origin origin;
    // Its creator's origin, that of the outside settings that its top-level
    // module graph is fetched with, even for a worker of an opaque origin.
    url::Origin outside_origin;
    // The origin that keys its agent cluster, its creator's.
    url::Origin agent_cluster_origin;
    // What its storage is keyed by: its creator's key, or its own opaque
    // origin's.
    std::string storage_key;
    std::u16string name;
    // The URL that new Worker() asked for, the key of a module worker's root
    // in its module map, which a redirect does not change.
    GURL script_url;
    bool module = false;
    // A module worker's credentials mode, which its graph's fetches take.
    FetchCredentialsMode credentials = FetchCredentialsMode::kSameOrigin;
    // Its creator's referrer policy, a window's document's, which a module
    // worker's top-level graph is fetched with, as through Blink's outside
    // settings. A worker's creator has none of its own: a Known Gap.
    FetchReferrerPolicy referrer_policy = FetchReferrerPolicy::kDefault;
    // Whether its creator is a worker, which holds the script to Blink's
    // strict check of its MIME type.
    bool strict_mime_type = false;
    // Whether its creator is a secure context.
    bool secure_context = true;
    // Whether it is cross-origin isolated: its creator's capability, but for
    // a worker of a data: URL, as DedicatedWorkerGlobalScope has it.
    bool cross_origin_isolated = false;
    // Whether it begins frames for requestAnimationFrame(), as a worker that
    // a window starts does, from the frame's compositor.
    bool begin_frames = false;
    // What its creator reads of the browser and the machine.
    PageStart start;
    PageConnection connection;
    bindings::RuntimeFeatureSet features =
        bindings::RuntimeFeatureSet::Stable();
    scoped_refptr<SessionSender> sender;
    base::RepeatingCallback<void(std::string why)> refuse;
    // Its creator's, used and let go of on the worker's thread.
    std::unique_ptr<DedicatedWorkerObjectProxy> object_proxy;
  };

  // The response of the worker's script, which its creator took.
  struct ScriptResponse {
    ScriptResponse();
    ScriptResponse(ScriptResponse&&);
    ScriptResponse& operator=(ScriptResponse&&);
    ~ScriptResponse();

    // The URL that it ended at.
    GURL url;
    int32_t status = 0;
    std::string headers;
    std::string body;
    // net::OK once all of the body came.
    int net_error = 0;
  };

  // Starts the thread, which makes the worker's isolate and context.
  explicit DedicatedWorkerThread(Params params);

  DedicatedWorkerThread(const DedicatedWorkerThread&) = delete;
  DedicatedWorkerThread& operator=(const DedicatedWorkerThread&) = delete;

  // Ends the worker's script at once if it still runs, and waits for the
  // thread to end.
  ~DedicatedWorkerThread() override;

  // Runs `response`'s script, from a task of the worker's, as
  // WorkerGlobalScope::DidFetchClassicScript() does once its checks pass.
  void RunClassicScript(ScriptResponse response);
  // Hands a message of its Worker's to the worker's global scope, from a task
  // of the worker's.
  void PostMessageToWorkerGlobalScope(SerializedScriptValue message,
                                      std::vector<MessagePortChannel> ports,
                                      bool user_activation);
  // Ends the worker, as WorkerThread::Terminate() does: what waits in its
  // queues never runs, and a script that still runs 2 seconds later is
  // stopped.
  void Terminate();
  // Stops a script that the worker runs, as the forced termination does.
  void TerminateExecution();
  // Signaled once the worker ends, which stops a synchronous request of its,
  // as Blink's sync-load abort event does.
  base::WaitableEvent& abort_synchronous_load() {
    return abort_synchronous_load_;
  }

  // base::PlatformThread::Delegate:
  void ThreadMain() override;

 private:
  class Scope;

  std::unique_ptr<WorkerScheduler> scheduler_;
  Params params_;
  scoped_refptr<base::SingleThreadTaskRunner> script_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> message_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> control_runner_;
  base::PlatformThreadHandle thread_;
  // What runs on the worker's thread, while it does. Only that thread uses
  // it.
  raw_ptr<Scope> scope_ = nullptr;
  base::Lock lock_;
  // The worker's isolate while the thread has one, for TerminateExecution().
  raw_ptr<v8::Isolate> isolate_ GUARDED_BY(lock_) = nullptr;
  bool terminating_ GUARDED_BY(lock_) = false;
  base::WaitableEvent abort_synchronous_load_;
};

}  // namespace mitto

#endif  // MITTO_PAGE_DEDICATED_WORKER_THREAD_H_
