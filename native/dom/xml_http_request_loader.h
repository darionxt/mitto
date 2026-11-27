// One request of an XMLHttpRequest's, as Blink's ThreadableLoader runs it for
// one: through the context's Fetcher, or answered by the Page for a data: or a
// blob: URL, with the timeout that the request has.
//
// Upstream: third_party/blink/renderer/core/loader/threadable_loader.h
// Upstream: third_party/blink/renderer/core/loader/threadable_loader.cc

#ifndef MITTO_DOM_XML_HTTP_REQUEST_LOADER_H_
#define MITTO_DOM_XML_HTTP_REQUEST_LOADER_H_

#include <stdint.h>

#include <string>

#include "base/functional/function_ref.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/local_scheme_fetch.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/visitor.h"

namespace mitto {

class ExecutionContext;
class Resource;
class XMLHttpRequest;

class XMLHttpRequestLoader final
    : public cppgc::GarbageCollected<XMLHttpRequestLoader> {
 public:
  XMLHttpRequestLoader(XMLHttpRequest* client, ExecutionContext* context);
  XMLHttpRequestLoader(const XMLHttpRequestLoader&) = delete;
  XMLHttpRequestLoader& operator=(const XMLHttpRequestLoader&) = delete;
  ~XMLHttpRequestLoader();

  void Trace(cppgc::Visitor* visitor) const;

  // How long the request may take from its start, or zero for as long as it
  // does. It may change while the request runs.
  void SetTimeout(base::TimeDelta timeout);
  // Starts the request, once.
  void Start(Fetcher& fetcher, FetchRequestData data);
  void Start(LocalSchemeFetch::Answer answer);
  // Takes what a link preloaded, with no request of its own.
  void Start(Resource& preload);
  // Fails a request that does not go anywhere, from a later task.
  void StartFailing(int net_error);
  // The same for a synchronous request, which runs to its end.
  void RunSynchronously(Fetcher& fetcher, FetchRequestData data);
  void RunSynchronously(LocalSchemeFetch::Answer answer);
  // Ends the request. The client hears nothing more.
  void Cancel();

 private:
  class RequestClient;

  // The request starts now.
  void DidStart();
  // Runs `run` in the context, which a synchronous request's caller is in.
  void Run(base::FunctionRef<void()> run);
  // Runs `call` with the client in its context, unless the request has ended.
  void CallClient(base::FunctionRef<void(XMLHttpRequest&)> call);
  void DidEnd(int net_error);
  // The same from a later task.
  void EndLater(int net_error);
  void DidTimeout();
  void StartTimer(base::TimeDelta delay);
  // The request has ended: the timer stops, and the client hears no more.
  void Clear();

  cppgc::Member<XMLHttpRequest> client_;
  cppgc::WeakMember<ExecutionContext> context_;
  // Whether the request runs within send(), in its context.
  bool synchronous_ = false;
  // Whether an asynchronous one is being handed to the Fetcher.
  bool starting_ = false;
  // What the request can be told, while it is open.
  raw_ptr<FetchController> controller_ = nullptr;
  base::TimeDelta timeout_;
  // Null before the request starts and once it has ended.
  base::TimeTicks request_started_;
  // The timer of the timeout that runs: a later one stops it.
  uint64_t timer_ = 0;
};

}  // namespace mitto

#endif  // MITTO_DOM_XML_HTTP_REQUEST_LOADER_H_
