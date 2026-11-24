#include "mitto/page/page_requests.h"

#include <deque>
#include <utility>
#include <variant>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/memory/stack_allocated.h"
#include "base/notreached.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/thread_annotations.h"
#include "mitto/dom/dedicated_worker_global_scope.h"
#include "mitto/dom/dedicated_worker_messaging.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/window.h"
#include "mitto/header_rules.h"
#include "mitto/page/resource_timing.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

namespace {

// Enters the context that a request of the Page's was made in, so that its
// client hears of it there, as Blink's ResourceFetcher runs a resource's
// clients in its ExecutionContext. A navigation has none: its loader enters
// its frame's current window's context.
class RequestContextScope final {
  STACK_ALLOCATED();

 public:
  // `context` is the request's, or null for a navigation.
  RequestContextScope(v8::Isolate* isolate,
                      const v8::Global<v8::Context>* context) {
    if (context && !context->IsEmpty()) {
      context_ = context->Get(isolate);
      context_->Enter();
    }
  }

  RequestContextScope(const RequestContextScope&) = delete;
  RequestContextScope& operator=(const RequestContextScope&) = delete;

  ~RequestContextScope() {
    if (!context_.IsEmpty()) {
      context_->Exit();
    }
  }

 private:
  v8::Local<v8::Context> context_;
};

}  // namespace

PageRequests::Timed::Timed() = default;
PageRequests::Timed::Timed(Timed&&) = default;
PageRequests::Timed& PageRequests::Timed::operator=(Timed&&) = default;
PageRequests::Timed::~Timed() = default;

PageRequests::Open::Open() = default;
PageRequests::Open::Open(Open&&) = default;
PageRequests::Open& PageRequests::Open::operator=(Open&&) = default;
PageRequests::Open::~Open() = default;

PageRequests::PageRequests(
    scoped_refptr<SessionSender> sender,
    base::RepeatingCallback<void(std::string why)> refuse,
    SessionSender::ResponseRoute route)
    : sender_(std::move(sender)),
      refuse_(std::move(refuse)),
      route_(std::move(route)) {}

PageRequests::~PageRequests() = default;

void PageRequests::SetNavigationClient(Client& client) {
  navigation_client_ = &client;
}

uint64_t PageRequests::Start(PageRequestKind kind,
                             std::string url,
                             Client& client,
                             bool render_blocking) {
  PageRequest request;
  request.kind = kind;
  request.url = std::move(url);
  return Start(std::move(request), client, render_blocking);
}

uint64_t PageRequests::Start(PageRequest request,
                             Client& client,
                             bool render_blocking,
                             std::u16string initiator_type) {
  // ResourceFetcher::RecordResourceTimingInfo(), when the page asks.
  Timed timed;
  timed.start_time = base::TimeTicks::Now();
  timed.outside_settings = request.kind == PageRequestKind::kWorker &&
                           request.mode == PageRequestMode::kCors;
  // A module's initiator type is "script" too, as ModuleScriptLoader sets it,
  // and so is that of a module of a worker's top-level graph.
  if (initiator_type.empty()) {
    initiator_type = request.kind == PageRequestKind::kScript ||
                             request.kind == PageRequestKind::kModule ||
                             request.kind == PageRequestKind::kModuleJson ||
                             timed.outside_settings
                         ? u"script"
                         : u"fetch";
  }
  timed.initiator_type = std::move(initiator_type);
  timed.url = base::UTF8ToUTF16(request.url);
  timed.render_blocking = render_blocking;
  ExecutionContext& context = ExecutionContext::Current();
  timed.execution_context = &context;
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  timed.context.Reset(isolate, isolate->GetCurrentContext());
  // The Session works out the Initiator from the context that asks.
  const uint64_t number = Send(context.ContextId(), request);
  Open& open = open_[number];
  open.client = &client;
  open.streams_body = request.streams_body;
  open.timed = std::move(timed);
  // What Blink's ThreadableLoader loads: fetch(), XMLHttpRequest, and the
  // favicon that Chrome's renderer fetches for the browser.
  open.script_request = request.kind == PageRequestKind::kFetch ||
                        request.kind == PageRequestKind::kXhr ||
                        request.kind == PageRequestKind::kFavicon;
  return number;
}

uint64_t PageRequests::StartWorkerScript(PageRequest request, Client& client) {
  const uint64_t number = Start(std::move(request), client,
                                /*render_blocking=*/false, u"other");
  open_[number].timed->worker_script = true;
  return number;
}

uint64_t PageRequests::StartNavigation(PageRequest request, Client& client) {
  const uint64_t number = Send(Window::Current().DocumentId(), request);
  open_[number].client = &client;
  open_[number].navigation = true;
  return number;
}

uint64_t PageRequests::StartTraversal(PageTraversalRequest traversal,
                                      Client& client) {
  const uint64_t context = Window::Current().DocumentId();
  const uint64_t number = sender_->SendRequest(
      context,
      [&](uint64_t number) {
        traversal.number = number;
        return EncodeTraversalRequest(context, traversal);
      },
      route_);
  // A navigation, whose response the Session says the cross-origin isolation
  // of first, as of any navigation, or which it answers with a restore.
  open_[number].client = &client;
  open_[number].navigation = true;
  open_[number].may_restore = traversal.stored_entry;
  return number;
}

uint64_t PageRequests::Send(uint64_t context, PageRequest& request) {
  // Its response can come only once this thread is done with this task, so
  // it finds the request open.
  return Send(context, request, route_);
}

uint64_t PageRequests::Send(uint64_t context,
                            PageRequest& request,
                            const SessionSender::ResponseRoute& route) {
  return sender_->SendRequest(
      context,
      [&](uint64_t number) {
        request.number = number;
        return EncodeRequest(context, request);
      },
      route);
}

namespace {

// What the I/O thread hands a synchronous request: each frame of its
// response, for the thread that waits for them.
class SynchronousResponse
    : public base::RefCountedThreadSafe<SynchronousResponse> {
 public:
  SynchronousResponse() = default;
  SynchronousResponse(const SynchronousResponse&) = delete;
  SynchronousResponse& operator=(const SynchronousResponse&) = delete;

  void Add(FrameFromSession frame) {
    {
      const base::AutoLock lock(lock_);
      frames_.push_back(std::move(frame));
    }
    arrived_.Signal();
  }

  std::deque<FrameFromSession> Take() {
    const base::AutoLock lock(lock_);
    std::deque<FrameFromSession> frames;
    frames.swap(frames_);
    return frames;
  }

  base::WaitableEvent& arrived() { return arrived_; }

 private:
  friend class base::RefCountedThreadSafe<SynchronousResponse>;
  ~SynchronousResponse() = default;

  base::Lock lock_;
  std::deque<FrameFromSession> frames_ GUARDED_BY(lock_);
  base::WaitableEvent arrived_{base::WaitableEvent::ResetPolicy::AUTOMATIC};
};

}  // namespace

void PageRequests::StartSynchronous(PageRequest request,
                                    Client& client,
                                    base::WaitableEvent& abort) {
  auto response = base::MakeRefCounted<SynchronousResponse>();
  const uint64_t number =
      Send(ExecutionContext::Current().ContextId(), request,
           base::BindRepeating(&SynchronousResponse::Add, response));
  open_[number].client = &client;
  while (open_.contains(number)) {
    base::WaitableEvent* events[] = {&response->arrived(), &abort};
    if (base::WaitableEvent::WaitMany(events) == 1) {
      Cancel(number);
      return;
    }
    for (FrameFromSession& frame : response->Take()) {
      switch (frame.type) {
        case PageMessage::kResponseStarted:
          OnResponseStarted(frame.response);
          break;
        case PageMessage::kResponseBody: {
          // The client takes the body as it comes.
          const size_t size = frame.body.bytes.size();
          OnResponseBody(std::move(frame.body));
          TookBody(number, size);
          break;
        }
        case PageMessage::kResponseEnded:
          OnResponseEnded(frame.ended);
          break;
        default:
          NOTREACHED();
      }
    }
  }
}

void PageRequests::Cancel(uint64_t number) {
  const auto found = open_.find(number);
  if (found == open_.end() || !found->second.client) {
    return;
  }
  // ResourceFetcher::HandleLoaderError() adds the entry of a request that the
  // page let go, as of now, but for a worker's script, whose worker never
  // loads it and so gets none, even if the Session has finished the response.
  if (found->second.timed) {
    if (!found->second.timed->worker_script) {
      AddResourceTiming(*found->second.timed, base::TimeTicks::Now(), nullptr);
    }
    found->second.timed.reset();
  }
  // It stays open until the Session ends it, so that what the Session sent
  // meanwhile still counts against its credit.
  found->second.client = nullptr;
  sender_->SendForRequest(number, EncodeCancel(number));
}

void PageRequests::CancelAll() {
  std::vector<uint64_t> numbers;
  for (const auto& [number, open] : open_) {
    if (open.client) {
      numbers.push_back(number);
    }
  }
  for (const uint64_t number : numbers) {
    Cancel(number);
  }
}

void PageRequests::Hint(const PageNetworkHint& hint) {
  const uint64_t document = Window::Current().DocumentId();
  sender_->SendFromContext(document, EncodeNetworkHint(document, hint));
}

void PageRequests::LetGo(uint64_t number) {
  const auto found = open_.find(number);
  if (found == open_.end()) {
    return;
  }
  found->second.client = nullptr;
  found->second.timed.reset();
}

// static
void PageRequests::AddResourceTiming(Timed& timed,
                                     base::TimeTicks response_end,
                                     const PageResponseEnded* ended) {
  ExecutionContext* const execution_context = timed.execution_context.Get();
  if (!execution_context || !execution_context->HasContext()) {
    return;
  }
  // A request can end outside any context, as when the garbage collector takes
  // a Response that no script read.
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const v8::Context::Scope context_scope(timed.context.Get(isolate));
  const PageResponseStarted* const response =
      timed.response ? &*timed.response : nullptr;
  ResourceTimingInfo info = CreateResourceTimingInfo(
      timed.start_time, std::move(timed.url), response, response_end);
  if (response) {
    if (info.allow_timing_details) {
      info.last_redirect_end_time = TicksOf(response->last_redirect_end);
    }
    if (ended) {
      SetResourceTimingSizes(info, *response, ended->encoded_body_size,
                             ended->received_bytes, timed.decoded_body_size);
    }
  }
  info.render_blocking_status = timed.render_blocking;
  // The browser hands the renderer none of a worker script's steps.
  if (timed.worker_script) {
    info.has_timing = false;
    info.timing = ResourceLoadTiming();
    info.last_redirect_end_time = base::TimeTicks();
  }
  // Only a dedicated worker fetches through outside settings, whose entries go
  // to its creator's timeline, through its Worker's proxy.
  if (timed.outside_settings && !execution_context->IsWindow()) {
    if (WorkerObjectProxy* const proxy =
            static_cast<DedicatedWorkerGlobalScope*>(execution_context)
                ->GetObjectProxy()) {
      proxy->AddOutsideResourceTiming(std::move(info), timed.initiator_type);
    }
    return;
  }
  execution_context->Performance()->AddResourceTiming(std::move(info),
                                                      timed.initiator_type);
}

// static
bool PageRequests::WorkerScriptLoaded(const Timed& timed,
                                      const PageResponseEnded& ended) {
  // net::OK.
  return ended.net_error == 0 && timed.response &&
         timed.response->status >= 200 && timed.response->status < 300;
}

void PageRequests::TookBody(uint64_t number, size_t bytes) {
  const auto found = open_.find(number);
  // Once a request has ended, the Session needs no more credit for it.
  if (found == open_.end() || bytes == 0) {
    return;
  }
  CHECK_LE(bytes, found->second.untaken);
  found->second.untaken -= bytes;
  PageCredit credit;
  credit.number = number;
  credit.bytes = base::checked_cast<uint32_t>(bytes);
  sender_->SendForRequest(credit.number, EncodeCredit(credit));
}

size_t PageRequests::BodyCredit(uint64_t number) const {
  const auto found = open_.find(number);
  if (found == open_.end() || found->second.body_ended) {
    return 0;
  }
  return base::checked_cast<size_t>(found->second.body_credited -
                                    found->second.body_sent);
}

void PageRequests::SendBodyPart(uint64_t number, std::string_view bytes) {
  const auto found = open_.find(number);
  if (found == open_.end()) {
    return;
  }
  Open& open = found->second;
  CHECK(open.streams_body);
  CHECK(!open.body_ended);
  CHECK_LE(bytes.size(), open.body_credited - open.body_sent);
  open.body_sent += bytes.size();
  PageRequestBodyPart part;
  part.number = number;
  part.bytes = std::string(bytes);
  sender_->SendForRequest(number, EncodeRequestBodyPart(part));
}

void PageRequests::EndBody(uint64_t number, bool errored) {
  const auto found = open_.find(number);
  if (found == open_.end()) {
    return;
  }
  CHECK(found->second.streams_body);
  CHECK(!found->second.body_ended);
  found->second.body_ended = true;
  PageRequestBodyEnded ended;
  ended.number = number;
  ended.errored = errored;
  sender_->SendForRequest(number, EncodeRequestBodyEnded(ended));
}

void PageRequests::DeliverHeld() {
  // What a client does may hold more, as a nested run loop of a synchronous
  // request takes messages meanwhile, so this goes on until nothing that
  // waits is for a window that runs.
  bool delivered = true;
  while (delivered) {
    delivered = false;
    std::deque<std::pair<uint64_t, Held>> waiting;
    waiting.swap(held_);
    while (!waiting.empty()) {
      auto [number, message] = std::move(waiting.front());
      waiting.pop_front();
      // A request whose window is frozen again, or still, waits on.
      const auto found = open_.find(number);
      if (found != open_.end() && IsFrozen(found->second)) {
        held_.emplace_back(number, std::move(message));
        continue;
      }
      if (const auto count = held_count_.find(number);
          count != held_count_.end() && --count->second == 0) {
        held_count_.erase(count);
      }
      delivered = true;
      if (auto* const started = std::get_if<PageResponseStarted>(&message)) {
        ProcessResponseStarted(*started);
      } else if (auto* const body = std::get_if<PageResponseBody>(&message)) {
        ProcessResponseBody(std::move(*body));
      } else if (auto* const ended = std::get_if<PageResponseEnded>(&message)) {
        ProcessResponseEnded(*ended);
      } else {
        ProcessRequestBodyCredit(std::get<PageCredit>(message));
      }
    }
  }
}

void PageRequests::CancelAllOf(const ExecutionContext& context) {
  std::vector<uint64_t> numbers;
  for (const auto& [number, open] : open_) {
    if (open.client && open.timed &&
        open.timed->execution_context == &context) {
      numbers.push_back(number);
    }
  }
  for (const uint64_t number : numbers) {
    Cancel(number);
  }
}

bool PageRequests::IsFrozen(const Open& open) {
  return open.timed && open.timed->execution_context &&
         open.timed->execution_context->IsInBackForwardCache();
}

bool PageRequests::ShouldHold(uint64_t number) const {
  // What comes after a message that waits waits too, in order.
  if (held_count_.contains(number)) {
    return true;
  }
  const auto found = open_.find(number);
  return found != open_.end() && IsFrozen(found->second);
}

void PageRequests::HoldMessage(uint64_t number, Held message) {
  held_.emplace_back(number, std::move(message));
  ++held_count_[number];
}

PageRequests::Open* PageRequests::Find(uint64_t number,
                                       std::string_view message) {
  if (number == kNavigationRequest && !navigation_seen_ && navigation_client_) {
    // The Session makes the navigation itself, at most once.
    navigation_seen_ = true;
    open_[kNavigationRequest].client = navigation_client_;
    open_[kNavigationRequest].navigation = true;
  }
  const auto found = open_.find(number);
  if (found == open_.end()) {
    refuse_.Run(
        base::StrCat({message, " of request ", base::NumberToString(number),
                      ", which is not in progress"}));
    return nullptr;
  }
  return &found->second;
}

void PageRequests::OnRestore(uint64_t number) {
  Open* const open = Find(number, "a restore");
  if (!open) {
    return;
  }
  if (!open->may_restore || open->started || open->isolation_said) {
    refuse_.Run(base::StrCat(
        {"a restore of request ", base::NumberToString(number),
         ", which is no traversal to a document that the back/forward cache "
         "keeps"}));
    return;
  }
  // Nothing more comes of it, and one that the Page let go hears nothing.
  Client* const client = open->client;
  open_.erase(number);
  if (client) {
    client->OnRestore();
  }
}

void PageRequests::OnIsolation(const PageIsolation& isolation) {
  Open* const open = Find(isolation.navigation, "a cross-origin isolation");
  if (!open) {
    return;
  }
  if (!open->navigation || open->started || open->isolation_said) {
    refuse_.Run(base::StrCat(
        {"a cross-origin isolation of request ",
         base::NumberToString(isolation.navigation),
         ", which is not a navigation whose response has yet to start"}));
    return;
  }
  open->isolation_said = true;
  if (open->client) {
    open->client->OnIsolation(isolation.isolation);
  }
}

void PageRequests::OnResponseStarted(const PageResponseStarted& response) {
  if (ShouldHold(response.number)) {
    HoldMessage(response.number, response);
    return;
  }
  ProcessResponseStarted(response);
}

void PageRequests::ProcessResponseStarted(const PageResponseStarted& response) {
  Open* const open = Find(response.number, "a response");
  if (!open) {
    return;
  }
  if (open->started) {
    refuse_.Run(base::StrCat({"a second response of request ",
                              base::NumberToString(response.number)}));
    return;
  }
  open->started = true;
  // ThreadableLoader::ResponseReceived(): a script's request that gets a
  // response with Cache-Control: no-store keeps a no-store page of its
  // context out of the back/forward cache.
  if (open->script_request && open->timed && open->timed->execution_context &&
      CacheControlContainsNoStore(
          HeaderField(response.headers, "cache-control"))) {
    open->timed->execution_context->DidReceiveNoStoreForScriptRequest();
  }
  if (open->timed) {
    open->timed->response = response;
    // WorkerMainScriptLoader starts once the browser hands the response over.
    // Chrome's starts once the worker's thread has, which often puts a small
    // script's end before its start: a Known Gap.
    if (open->timed->worker_script) {
      open->timed->start_time = base::TimeTicks::Now();
    }
  }
  if (open->client) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handle_scope(isolate);
    const RequestContextScope context_scope(
        isolate, open->timed ? &open->timed->context : nullptr);
    open->client->OnResponseStarted(response);
  }
}

void PageRequests::OnResponseBody(PageResponseBody body) {
  if (ShouldHold(body.number)) {
    const uint64_t number = body.number;
    HoldMessage(number, std::move(body));
    return;
  }
  ProcessResponseBody(std::move(body));
}

void PageRequests::ProcessResponseBody(PageResponseBody body) {
  Open* const open = Find(body.number, "a body");
  if (!open) {
    return;
  }
  if (!open->started || body.bytes.size() > kBodyWindow - open->untaken) {
    refuse_.Run(base::StrCat({"more of the body of request ",
                              base::NumberToString(body.number),
                              " than the Page has given credit for"}));
    return;
  }
  if (!open->client) {
    // Cancelled: the Session needs no credit for what it sent meanwhile.
    return;
  }
  if (open->timed) {
    open->timed->decoded_body_size += body.bytes.size();
  }
  open->untaken += body.bytes.size();
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  const v8::HandleScope handle_scope(isolate);
  const RequestContextScope context_scope(
      isolate, open->timed ? &open->timed->context : nullptr);
  // The client may take the bytes, and give credit, before this returns.
  open->client->OnResponseBody(std::move(body.bytes));
}

void PageRequests::OnRequestBodyCredit(const PageCredit& credit) {
  if (ShouldHold(credit.number)) {
    HoldMessage(credit.number, credit);
    return;
  }
  ProcessRequestBodyCredit(credit);
}

void PageRequests::ProcessRequestBodyCredit(const PageCredit& credit) {
  Open* const open = Find(credit.number, "credit for the body");
  if (!open) {
    return;
  }
  // The Session gives credit for one window at first, and after that for
  // what it has taken.
  if (!open->streams_body ||
      credit.bytes > open->body_sent + kBodyWindow - open->body_credited) {
    refuse_.Run(base::StrCat({"credit for more of the body of request ",
                              base::NumberToString(credit.number),
                              " than the Page sent"}));
    return;
  }
  open->body_credited += credit.bytes;
  if (open->client && !open->body_ended) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handle_scope(isolate);
    const RequestContextScope context_scope(
        isolate, open->timed ? &open->timed->context : nullptr);
    open->client->OnRequestBodyCredit();
  }
}

void PageRequests::OnResponseEnded(const PageResponseEnded& ended) {
  if (ShouldHold(ended.number)) {
    HoldMessage(ended.number, ended);
    return;
  }
  ProcessResponseEnded(ended);
}

void PageRequests::ProcessResponseEnded(const PageResponseEnded& ended) {
  Open* const open = Find(ended.number, "the end");
  if (!open) {
    return;
  }
  Client* const client = open->client;
  std::optional<Timed> timed = std::move(open->timed);
  open_.erase(ended.number);
  // ResourceFetcher adds the entry before the resource's clients hear of the
  // end, so that a script sees its own. A request that the Session refused
  // before it started never loaded, as Blink fails such a fetch first.
  if (timed && ended.completion_time != 0 &&
      (!timed->worker_script || WorkerScriptLoaded(*timed, ended))) {
    AddResourceTiming(*timed, TicksOf(ended.completion_time), &ended);
  }
  if (client) {
    v8::Isolate* const isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope handle_scope(isolate);
    const RequestContextScope context_scope(isolate,
                                            timed ? &timed->context : nullptr);
    client->OnResponseTotals(ended);
    client->OnResponseEnded(ended.net_error);
  }
}

}  // namespace mitto
