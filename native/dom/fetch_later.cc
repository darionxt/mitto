#include "mitto/dom/fetch_later.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "mitto/bindings/script_wrappable.h"
#include "mitto/dom/abort_signal.h"
#include "mitto/dom/body_stream_buffer.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/fetch_later_result.h"
#include "mitto/dom/fetcher.h"
#include "mitto/dom/frame.h"
#include "mitto/dom/headers.h"
#include "mitto/dom/pending_script.h"
#include "mitto/dom/quota_exceeded_error.h"
#include "mitto/dom/request.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_tasks.h"
#include "net/base/url_util.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace mitto {
namespace {

// What fetch_later_util.h allows: a top-level document, which holds the
// minimal quota of its frames apart, each origin of a request, and the
// element of a frame, which reserves the normal or the minimal quota.
constexpr uint64_t kMaxScheduledDeferredBytes = 640 * 1024;
constexpr uint64_t kQuotaReservedForDeferredFetchMinimal = 128 * 1024;
constexpr uint64_t kMaxPerRequestOriginScheduledDeferredBytes = 64 * 1024;
constexpr uint64_t kNormalReservedDeferredFetchQuota = 64 * 1024;
constexpr uint64_t kMinimalReservedDeferredFetchQuota = 8 * 1024;
constexpr uint64_t kMaxContainersWithMinimalQuota =
    kQuotaReservedForDeferredFetchMinimal / kMinimalReservedDeferredFetchQuota;

// FetchLaterUtil::AreSameOrigin(): whether the documents of two frames are of
// one origin.
bool AreSameOrigin(const Frame& a, const Frame& b) {
  const Window* const window_a = a.DomWindow();
  const Window* const window_b = b.DomWindow();
  return window_a && window_b &&
         window_a->GetSecurityOrigin().IsSameOriginWith(
             window_b->GetSecurityOrigin());
}

// Whether "deferred-fetch", whose default allowlist is 'self', is enabled in
// the document of `frame`. A Page takes neither a Permissions-Policy header
// nor an allow attribute, so it is in the top-level document and, as
// PermissionsPolicy::InheritedValueForFeature() gives it, in each frame's
// document of its parent's origin where it is in the parent's.
// "deferred-fetch-minimal", whose default allowlist is *, is in each.
bool IsDeferredFetchAllowed(const Frame& frame) {
  for (const Frame* child = &frame; child->Parent(); child = child->Parent()) {
    if (!AreSameOrigin(*child, *child->Parent())) {
      return false;
    }
  }
  return true;
}

// FetchLaterUtil::GetDeferredFetchControlFrame(): the frame itself, if it is
// top-level or of another origin than its parent, else its parent's.
const Frame& DeferredFetchControlFrame(const Frame& frame) {
  const Frame* control = &frame;
  while (control->Parent() && AreSameOrigin(*control, *control->Parent())) {
    control = control->Parent();
  }
  return *control;
}

// FetchLaterUtil::ToReservedDeferredFetchQuota().
uint64_t ToReservedDeferredFetchQuota(DeferredFetchPolicy policy) {
  switch (policy) {
    case DeferredFetchPolicy::kDisabled:
      return 0;
    case DeferredFetchPolicy::kDeferredFetch:
      return kNormalReservedDeferredFetchQuota;
    case DeferredFetchPolicy::kDeferredFetchMinimal:
      return kMinimalReservedDeferredFetchQuota;
  }
}

// Runs `visit` on each descendant of `frame`, in the order of Blink's
// FrameTree::TraverseNext() within it.
template <typename Visit>
void ForEachDescendant(const Frame& frame, const Visit& visit) {
  for (size_t index = 0; index < frame.ChildCount(); ++index) {
    const Frame& child = *frame.ChildAt(index);
    visit(child);
    ForEachDescendant(child, visit);
  }
}

// FetchLaterUtil::GetReservedDeferredFetchQuota() of a control frame: 512 KiB
// for a top-level one, and otherwise what its element reserves, if the
// document may use it.
uint64_t ReservedDeferredFetchQuota(const Frame& frame) {
  if (!frame.Parent()) {
    return kMaxScheduledDeferredBytes - kQuotaReservedForDeferredFetchMinimal;
  }
  const HTMLFrameOwnerElement* const owner = frame.Owner();
  if (!owner) {
    return 0;
  }
  const uint64_t reserved =
      ToReservedDeferredFetchQuota(owner->GetDeferredFetchPolicy());
  if (reserved == kNormalReservedDeferredFetchQuota &&
      IsDeferredFetchAllowed(frame)) {
    return reserved;
  }
  return reserved == kMinimalReservedDeferredFetchQuota ? reserved : 0;
}

// FetchLaterUtil::GetAvailableDeferredFetchQuota(): what is left of the quota
// of the control frame of `frame`, less what each element under a frame of
// that control frame reserves, and what the deferred requests of their
// windows take, for a request to `origin`, of which at most 64 KiB.
uint64_t AvailableDeferredFetchQuota(const Frame& frame,
                                     const url::Origin& origin) {
  const Frame& control = DeferredFetchControlFrame(frame);
  uint64_t quota = ReservedDeferredFetchQuota(control);
  uint64_t quota_for_request_origin =
      kMaxPerRequestOriginScheduledDeferredBytes;
  const auto take = [&](const Frame& navigable) {
    if (&DeferredFetchControlFrame(navigable) != &control) {
      return;
    }
    // Each element under it, at any depth, as Blink walks them.
    ForEachDescendant(navigable, [&quota](const Frame& container) {
      if (const HTMLFrameOwnerElement* const owner = container.Owner()) {
        quota -= std::min(quota, ToReservedDeferredFetchQuota(
                                     owner->GetDeferredFetchPolicy()));
      }
    });
    if (const Window* const window = navigable.DomWindow()) {
      if (const DeferredFetches* const records =
              window->FindDeferredFetches()) {
        records->TakeFromQuota(origin, quota_for_request_origin, quota);
      }
    }
  };
  take(control);
  ForEachDescendant(control, take);
  return std::min(quota, quota_for_request_origin);
}

// network::IsUrlPotentiallyTrustworthy(): a secure scheme, or a host that is
// the machine itself.
bool IsUrlPotentiallyTrustworthy(const GURL& url) {
  return url.SchemeIs(url::kHttpsScheme) ||
         (url.SchemeIs(url::kHttpScheme) && net::IsLocalhost(url));
}

// FetchLaterUtil::CalculateRequestSize(): the URL without its fragment, the
// referrer as the request holds it, "about:client" by default, each header
// line's name and value, and the body.
uint64_t RequestSize(const Request& request, size_t body_size) {
  GURL::Replacements no_fragment;
  no_fragment.ClearRef();
  uint64_t size = request.url().ReplaceComponents(no_fragment).spec().size() +
                  request.referrer().size() + body_size;
  for (const auto& [name, value] : request.GetHeaders()->list()) {
    size += name.size() + value.size();
  }
  return size;
}

class DeferredClient;

// The abort algorithm of a deferred request, which holds its client weakly,
// as Blink's binds the DeferredLoader's Abort().
class AbortDeferredAlgorithm final : public AbortSignal::Algorithm {
 public:
  explicit AbortDeferredAlgorithm(base::WeakPtr<DeferredClient> client)
      : client_(std::move(client)) {}

  void Run() override;

 private:
  base::WeakPtr<DeferredClient> client_;
};

// Blink's DeferredLoader: holds the request until its activateAfter has
// passed, or the back/forward cache takes its document, which has the
// Session send it, or its signal aborts, which cancels it, and otherwise lets
// it go with its document, which the Session then sends. Its response never
// reaches the page.
class DeferredClient final : public FetchClient {
 public:
  DeferredClient(FetchLaterResult* result,
                 scoped_refptr<DeferredFetches> records,
                 AbortSignal* signal,
                 std::optional<base::TimeDelta> activate_after)
      : result_(result),
        records_(std::move(records)),
        signal_(signal),
        activate_after_(activate_after) {}

  DeferredClient(const DeferredClient&) = delete;
  DeferredClient& operator=(const DeferredClient&) = delete;

  ~DeferredClient() override {
    if (record_) {
      records_->Remove(record_);
    }
    // Its document went, or its Page ends, which sends it, as
    // FetchLaterManager's ContextDestroyed() processes each DeferredLoader.
    if (state_ == State::kPending) {
      result_->SetActivated();
    }
  }

  // What it counts against the quota, as the window's records hold it.
  void SetRecord(uint64_t record) { record_ = record; }
  base::WeakPtr<DeferredClient> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // DeferredLoader::Process(): the Session sends it now, and its result says
  // it has been activated.
  void Send() {
    if (state_ != State::kPending) {
      return;
    }
    state_ = State::kSent;
    result_->SetActivated();
    // Destroys this.
    controller_->SendDeferred();
  }

  // FetchClient:
  void OnStarted(FetchController& controller) override {
    controller_ = &controller;
    signal_->AddAlgorithm(
        bindings::MakeGarbageCollected<AbortDeferredAlgorithm>(
            weak_factory_.GetWeakPtr()));
    // DeferredLoader's timer, which stops as the request ends.
    if (activate_after_) {
      timer_.SetTaskRunner(
          EventLoop::Get()->TaskRunner(TaskType::kNetworkingUnfreezable));
      timer_.Start(
          FROM_HERE, *activate_after_,
          base::BindOnce(&DeferredClient::Send, weak_factory_.GetWeakPtr()));
    }
  }
  void OnResponseStarted(const FetchResponseHead& head) override {}
  void OnResponseBody(std::string bytes) override {}
  // A request that cannot be sent, as one too large for a frame, or one that
  // the signal cancelled.
  void OnResponseEnded(int net_error) override {
    if (state_ == State::kPending) {
      state_ = State::kFailed;
      timer_.Stop();
    }
  }
  void OnDocumentGone() override {
    if (state_ == State::kPending) {
      state_ = State::kSent;
      timer_.Stop();
      result_->SetActivated();
    }
  }

  void Abort() {
    if (state_ != State::kPending) {
      return;
    }
    state_ = State::kAborted;
    timer_.Stop();
    // Destroys this.
    controller_->Cancel();
  }

 private:
  enum class State { kPending, kSent, kAborted, kFailed };

  cppgc::Persistent<FetchLaterResult> result_;
  const scoped_refptr<DeferredFetches> records_;
  uint64_t record_ = 0;
  cppgc::Persistent<AbortSignal> signal_;
  const std::optional<base::TimeDelta> activate_after_;
  raw_ptr<FetchController> controller_ = nullptr;
  base::OneShotTimer timer_;
  State state_ = State::kPending;
  base::WeakPtrFactory<DeferredClient> weak_factory_{this};
};

void AbortDeferredAlgorithm::Run() {
  if (DeferredClient* const client = client_.get()) {
    client->Abort();
  }
}

}  // namespace

DeferredFetches::DeferredFetches() = default;
DeferredFetches::~DeferredFetches() = default;

DeferredFetches::Record::Record() = default;
DeferredFetches::Record::Record(Record&&) = default;
DeferredFetches::Record& DeferredFetches::Record::operator=(Record&&) = default;
DeferredFetches::Record::~Record() = default;

uint64_t DeferredFetches::Add(const url::Origin& origin,
                              uint64_t bytes,
                              base::OnceClosure send) {
  const uint64_t id = next_id_++;
  Record& record = records_[id];
  record.origin = origin;
  record.bytes = bytes;
  record.send = std::move(send);
  return id;
}

void DeferredFetches::Remove(uint64_t id) {
  records_.erase(id);
}

void DeferredFetches::TakeFromQuota(const url::Origin& origin,
                                    uint64_t& for_origin,
                                    uint64_t& total) const {
  for (const auto& [id, record] : records_) {
    total -= std::min(total, record.bytes);
    if (record.origin.IsSameOriginWith(origin)) {
      for_origin -= std::min(for_origin, record.bytes);
    }
  }
}

void DeferredFetches::SendAll() {
  // Each send removes its record.
  std::vector<base::OnceClosure> sends;
  for (auto& [id, record] : records_) {
    sends.push_back(std::move(record.send));
  }
  for (base::OnceClosure& send : sends) {
    if (send) {
      std::move(send).Run();
    }
  }
}

DeferredFetchPolicy DeferredFetchPolicyOnNavigation(
    const Frame& frame,
    const url::Origin& to_origin) {
  const Frame* const parent = frame.Parent();
  CHECK(parent);
  // The normal quota, for a document of the parent's origin where the parent
  // may use deferred-fetch.
  const Window* const parent_window = parent->DomWindow();
  if (parent_window && IsDeferredFetchAllowed(*parent) &&
      to_origin.IsSameOriginWith(parent_window->GetSecurityOrigin())) {
    return DeferredFetchPolicy::kDeferredFetch;
  }
  // Otherwise the minimal quota, under a top-level control document that has
  // given fewer than 16 elements the minimal quota, the element's own
  // included.
  const Frame& control = DeferredFetchControlFrame(*parent);
  if (control.Parent()) {
    return DeferredFetchPolicy::kDisabled;
  }
  uint64_t minimal = 0;
  ForEachDescendant(control, [&minimal](const Frame& navigable) {
    if (const HTMLFrameOwnerElement* const owner = navigable.Owner();
        owner && owner->GetDeferredFetchPolicy() ==
                     DeferredFetchPolicy::kDeferredFetchMinimal) {
      ++minimal;
    }
  });
  return minimal < kMaxContainersWithMinimalQuota
             ? DeferredFetchPolicy::kDeferredFetchMinimal
             : DeferredFetchPolicy::kDisabled;
}

FetchLaterResult* FetchLater(Window& window,
                             const bindings::RequestOrUSVString& input,
                             const bindings::DeferredRequestInit& init,
                             bindings::ExceptionState& exception_state) {
  // GlobalFetch's ScopedFetcher: a window that the Page has let go, such as a
  // detached frame's, whose fetchLater() a script kept, before the Request
  // takes its input.
  if (!window.HasContext()) {
    exception_state.ThrowTypeError("The global scope is shutting down.");
    return nullptr;
  }
  Request* const request = Request::Create(input, init, exception_state);
  if (!request) {
    return nullptr;
  }
  BodyStreamBuffer* const body = request->PassBody(exception_state);
  if (exception_state.HadException()) {
    return nullptr;
  }
  AbortSignal* const signal = request->Signal();
  if (signal->Aborted()) {
    exception_state.ThrowDOMException("AbortError",
                                      "The user aborted a fetchLater request.");
    return nullptr;
  }
  std::optional<base::TimeDelta> activate_after;
  if (init.activate_after) {
    activate_after = base::Milliseconds(*init.activate_after);
    if (activate_after->is_negative()) {
      exception_state.ThrowRangeError(
          "fetchLater's activateAfter cannot be negative.");
      return nullptr;
    }
  }
  Fetcher* const fetcher = window.GetFetcher();
  WindowTasks* const tasks = window.GetTasks();
  const Frame* const frame = window.GetFrame();
  if (!fetcher || !tasks || !frame) {
    exception_state.ThrowDOMException(
        "InvalidStateError",
        "fetchLater can only be called from a fully active Document.");
    return nullptr;
  }
  const GURL& url = request->url();
  if (!url.SchemeIsHTTPOrHTTPS()) {
    exception_state.ThrowTypeError(
        "fetchLater is only supported over HTTP(S).");
    return nullptr;
  }
  if (!IsUrlPotentiallyTrustworthy(url)) {
    exception_state.ThrowDOMException("SecurityError",
                                      "fetchLater was passed an insecure URL.");
    return nullptr;
  }
  if (body && body->IsMadeFromReadableStream()) {
    exception_state.ThrowTypeError(
        "fetchLater doesn't support body with unknown length.");
    return nullptr;
  }
  const url::Origin origin = url::Origin::Create(url);
  // A string body counts by its length, as Blink's ExtractBody() records it,
  // though it goes as UTF-8.
  const uint64_t size = RequestSize(*request, request->body_length());
  const uint64_t available = AvailableDeferredFetchQuota(*frame, origin);
  if (available < size) {
    QuotaExceededError::Throw(
        exception_state,
        base::StrCat({"fetchLater exceeds its quota for the origin: got ",
                      base::NumberToString(size), " bytes, expected less than ",
                      base::NumberToString(available), " bytes."}));
    return nullptr;
  }
  // The body's bytes, once the quota has room for them.
  std::optional<std::string> bytes;
  if (body && request->Method() != "GET" && request->Method() != "HEAD") {
    bytes = body->DrainAsBytes(exception_state);
    if (exception_state.HadException()) {
      return nullptr;
    }
  }
  auto* const result = bindings::MakeGarbageCollected<FetchLaterResult>();
  // The Fetch Standard's main fetch, as DeferredLoader's FetchLoaderBase
  // starts it: a cross-origin request of mode same-origin, and one of mode
  // no-cors that would not follow a redirect, end before anything goes, and
  // its result is never activated.
  const bool cross_origin = !window.GetSecurityOrigin().IsSameOriginWith(url);
  if (cross_origin && request->mode() == FetchMode::kSameOrigin) {
    return result;
  }
  if (cross_origin && request->mode() == FetchMode::kNoCors &&
      request->redirect_mode() != FetchRedirectMode::kFollow) {
    window.AddConsoleMessage(
        ConsoleLevel::kError,
        base::StrCat({"Fetch API cannot load ", ElidedUrl(url),
                      ". Request mode is \"no-cors\" but the redirect mode "
                      "is not \"follow\"."}));
    return result;
  }
  FetchRequestData data;
  data.body = std::move(bytes);
  data.method = request->Method();
  data.url = url.spec();
  data.headers = request->GetHeaders()->ToLines();
  if (request->mode() != FetchMode::kCors) {
    data.mode = request->mode();
  }
  if (request->credentials() != FetchCredentialsMode::kSameOrigin) {
    data.credentials = request->credentials();
  }
  // A referrer that is no HTTP or HTTPS URL sends none, as for fetch().
  data.referrer = request->referrer();
  if (data.referrer != kClientReferrer &&
      !GURL(data.referrer).SchemeIsHTTPOrHTTPS()) {
    data.referrer = kNoReferrer;
  }
  data.referrer_policy =
      request->referrer_policy() == FetchReferrerPolicy::kDefault
          ? window.GetReferrerPolicy()
          : request->referrer_policy();
  data.priority = ComputeFetchPriority(/*lazy=*/false, request->priority(),
                                       /*render_blocking=*/false);
  data.keepalive = true;
  data.deferred = true;
  data.cache_mode = request->cache_mode();
  data.redirect_mode = request->redirect_mode();
  const scoped_refptr<DeferredFetches>& records = window.GetDeferredFetches();
  auto client =
      std::make_unique<DeferredClient>(result, records, signal, activate_after);
  client->SetRecord(records->Add(
      origin, size,
      base::BindOnce(&DeferredClient::Send, client->GetWeakPtr())));
  fetcher->Fetch(std::move(data), std::move(client));
  return result;
}

}  // namespace mitto
