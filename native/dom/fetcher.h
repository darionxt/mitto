// What a window's fetch() asks of the Page program: requests through the
// Session that loaded the window's document, with the document as their
// Initiator. The Page program gives the window one, and the DOM knows nothing
// of how it reaches the Session, as with the CookieJar.

#ifndef MITTO_DOM_FETCHER_H_
#define MITTO_DOM_FETCHER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mitto {

// The Fetch Standard's request destination: none for fetch().
enum class FetchDestination {
  kEmpty,
  kScript,
};

// The Fetch Standard's request mode, of those that a page's requests have.
enum class FetchMode {
  kCors,
  kSameOrigin,
  kNoCors,
};

// The Fetch Standard's credentials mode.
enum class FetchCredentialsMode {
  kOmit,
  kSameOrigin,
  kInclude,
};

// A referrer policy, where kDefault, the empty string, leaves the document's.
enum class FetchReferrerPolicy {
  kDefault,
  kNoReferrer,
  kNoReferrerWhenDowngrade,
  kOrigin,
  kOriginWhenCrossOrigin,
  kSameOrigin,
  kStrictOrigin,
  kStrictOriginWhenCrossOrigin,
  kUnsafeUrl,
};

// The priority that a request goes at, as Blink's ResourceLoadPriority has
// it, lowest first.
enum class FetchPriority {
  kVeryLow,
  kLow,
  kMedium,
  kHigh,
  kVeryHigh,
};

// A fetch priority hint, as fetch()'s priority or an element's fetchpriority
// gives it.
enum class FetchPriorityHint {
  kAuto,
  kLow,
  kHigh,
};

// The priority that Blink's ResourceFetcher gives a request whose type's own is
// kHigh, as fetch()'s and a classic script's is: kLow if it is lazy, as an
// async, deferred or script-inserted script is, then moved by `hint`, and by
// blocking rendering. Returns nullopt for kHigh, the destination's own.
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_fetcher.cc
// Upstream: third_party/blink/renderer/platform/loader/fetch/resource_request_utils.cc
std::optional<FetchPriority> ComputeFetchPriority(bool lazy,
                                                  FetchPriorityHint hint,
                                                  bool render_blocking);

// The hint that a fetchpriority attribute's value gives, in any case: auto for
// any but "high" and "low".
// Upstream: third_party/blink/renderer/core/loader/fetch_priority_attribute.cc
FetchPriorityHint FetchPriorityHintFromAttribute(std::u16string_view value);

// The referrer policy that a referrerpolicy attribute's value or a
// RequestInit's referrerPolicy names, in any case, without the legacy
// keywords: kDefault for any other.
// Upstream: third_party/blink/renderer/platform/weborigin/security_policy.cc
FetchReferrerPolicy FetchReferrerPolicyFromString(std::u16string_view value);

// The referrer of a request that is the document's, as Blink's referrer string
// says it, and of one that has none.
inline constexpr char kClientReferrer[] = "about:client";
inline constexpr char kNoReferrer[] = "";

// A request of a page's, once fetch() or a script element has checked it.
struct FetchRequestData {
  FetchRequestData();
  FetchRequestData(FetchRequestData&&);
  FetchRequestData& operator=(FetchRequestData&&);
  ~FetchRequestData();

  std::string method;
  // In UTF-8.
  std::string url;
  // The lines that the script set, each "name: value" and a line feed.
  std::string headers;
  std::optional<std::string> body;
  // The request's mode and credentials mode, if not those of its destination.
  std::optional<FetchMode> mode;
  std::optional<FetchCredentialsMode> credentials;
  // kClientReferrer, kNoReferrer, or a URL of the document's origin.
  std::string referrer = kClientReferrer;
  FetchReferrerPolicy referrer_policy = FetchReferrerPolicy::kDefault;
  // The priority, if not its destination's.
  std::optional<FetchPriority> priority;
  // Whether it outlives its document.
  bool keepalive = false;
  FetchDestination destination = FetchDestination::kEmpty;
  // Whether it blocks rendering, which its entry of the Performance Timeline
  // says.
  bool render_blocking = false;
};

// A response, after any redirects.
struct FetchResponseHead {
  FetchResponseHead();
  FetchResponseHead(const FetchResponseHead&);
  FetchResponseHead& operator=(const FetchResponseHead&);
  ~FetchResponseHead();

  int32_t status = 0;
  std::string status_text;
  // In UTF-8.
  std::string url;
  bool redirected = false;
  // Whether its tainting is cors, as the Session worked it out: a hop went to
  // another origin than the document's.
  bool cors = false;
  // Whether its tainting is opaque: a script's, of mode no-cors, whose hops
  // left the document's origin.
  bool opaque = false;
  // Each "name: value" and a line feed, without Set-Cookie.
  std::string headers;
};

// What a request can be told while it runs.
class FetchController {
 public:
  // The client has taken `bytes` more of the body, which lets the Session
  // send as many more.
  virtual void TookBody(size_t bytes) = 0;
  // Ends the request, and destroys its client before it returns, so the
  // client must touch nothing of itself after calling it.
  virtual void Cancel() = 0;

 protected:
  virtual ~FetchController() = default;
};

// Hears how one request goes. The Fetcher owns it, and destroys it after its
// end or on Cancel().
class FetchClient {
 public:
  virtual ~FetchClient() = default;

  // First, before anything else: what the client can tell the request.
  virtual void OnStarted(FetchController& controller) = 0;
  virtual void OnResponseStarted(const FetchResponseHead& head) = 0;
  // The next bytes of the body, which the client takes now or later through
  // TookBody().
  virtual void OnResponseBody(std::string bytes) = 0;
  // 0 when all of the body has come, and otherwise a net error, before or
  // after the response started.
  virtual void OnResponseEnded(int net_error) = 0;
};

class Fetcher {
 public:
  virtual void Fetch(FetchRequestData request,
                     std::unique_ptr<FetchClient> client) = 0;

 protected:
  virtual ~Fetcher() = default;
};

}  // namespace mitto

#endif  // MITTO_DOM_FETCHER_H_
