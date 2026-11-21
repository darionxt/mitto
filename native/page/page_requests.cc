#include "mitto/page/page_requests.h"

#include <deque>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/notreached.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/thread_annotations.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/performance.h"
#include "mitto/dom/window.h"
#include "mitto/page/resource_timing.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"

namespace mitto {

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
                             bool render_blocking) {
  // ResourceFetcher::RecordResourceTimingInfo(), when the page asks.
  Timed timed;
  timed.start_time = base::TimeTicks::Now();
  // A module's initiator type is "script" too, as ModuleScriptLoader sets it.
  timed.initiator_type = request.kind == PageRequestKind::kScript ||
                                 request.kind == PageRequestKind::kModule ||
                                 request.kind == PageRequestKind::kModuleJson
                             ? u"script"
                             : u"fetch";
  timed.url = base::UTF8ToUTF16(request.url);
  timed.render_blocking = render_blocking;
  ExecutionContext& context = ExecutionContext::Current();
  if (context.IsWindow()) {
    timed.window = &Window::Current();
  }
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  timed.context.Reset(isolate, isolate->GetCurrentContext());
  // The Session works out the Initiator from the context that asks.
  const uint64_t number = Send(context.ContextId(), request);
  Open& open = open_[number];
  open.client = &client;
  open.streams_body = request.streams_body;
  open.timed = std::move(timed);
  return number;
}

uint64_t PageRequests::StartUntimed(PageRequest request, Client& client) {
  const uint64_t number =
      Send(ExecutionContext::Current().ContextId(), request);
  open_[number].client = &client;
  return number;
}

uint64_t PageRequests::StartNavigation(PageRequest request, Client& client) {
  const uint64_t number = Send(Window::Current().DocumentId(), request);
  open_[number].client = &client;
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
  // page let go, as of now.
  if (found->second.timed) {
    AddResourceTiming(*found->second.timed, base::TimeTicks::Now(), nullptr);
    found->second.timed.reset();
  }
  // It stays open until the Session ends it, so that what the Session sent
  // meanwhile still counts against its credit.
  found->second.client = nullptr;
  sender_->Send(EncodeCancel(number));
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
  Window* const window = timed.window.Get();
  if (!window || !window->HasContext()) {
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
  window->Performance()->AddResourceTiming(std::move(info),
                                           timed.initiator_type);
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
  sender_->Send(EncodeCredit(credit));
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
  sender_->Send(EncodeRequestBodyPart(part));
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
  sender_->Send(EncodeRequestBodyEnded(ended));
}

PageRequests::Open* PageRequests::Find(uint64_t number,
                                       std::string_view message) {
  if (number == kNavigationRequest && !navigation_seen_ && navigation_client_) {
    // The Session makes the navigation itself, at most once.
    navigation_seen_ = true;
    open_[kNavigationRequest].client = navigation_client_;
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

void PageRequests::OnResponseStarted(const PageResponseStarted& response) {
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
  if (open->timed) {
    open->timed->response = response;
  }
  if (open->client) {
    open->client->OnResponseStarted(response);
  }
}

void PageRequests::OnResponseBody(PageResponseBody body) {
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
  // The client may take the bytes, and give credit, before this returns.
  open->client->OnResponseBody(std::move(body.bytes));
}

void PageRequests::OnRequestBodyCredit(const PageCredit& credit) {
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
    open->client->OnRequestBodyCredit();
  }
}

void PageRequests::OnResponseEnded(const PageResponseEnded& ended) {
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
  if (timed && ended.completion_time != 0) {
    AddResourceTiming(*timed, TicksOf(ended.completion_time), &ended);
  }
  if (client) {
    client->OnResponseTotals(ended);
    client->OnResponseEnded(ended.net_error);
  }
}

}  // namespace mitto
