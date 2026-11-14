// The library's side of a Page: its process and its end of the channel, on the
// network thread, and the events that wait for whichever thread holds the
// Page's handle.

#ifndef MITTO_PAGE_HOST_H_
#define MITTO_PAGE_HOST_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/memory/raw_ref.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "mitto/network_quality.h"
#include "mitto/page_channel.h"
#include "mitto/page_launcher.h"
#include "mitto/page_messages.h"
#include "mitto/request_client.h"
#include "mitto/request_options.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_change_dispatcher.h"
#include "net/cookies/cookie_partition_key.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_request_headers.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace net {
class CookieStore;
}  // namespace net

namespace mitto {

class CacheStorageContext;
class PageCacheStorage;
class PageHost;
class ReadyRequests;
class StorageQuota;

// What a request of one Request Kind sends, as the Session's Persona says: its
// header lines in their order, and its options, with no Initiator, which the
// library sets to the Page's document.
struct PageRequestTemplate {
  PageRequestTemplate();
  PageRequestTemplate(const PageRequestTemplate&);
  PageRequestTemplate& operator=(const PageRequestTemplate&);
  ~PageRequestTemplate();

  net::HttpRequestHeaders headers;
  RequestOptions options;
};

// What mitto_page_open() reads.
struct PageOptions {
  PageOptions();
  PageOptions(PageOptions&&);
  PageOptions& operator=(PageOptions&&);
  ~PageOptions();

  PageStart start;
  // Where the Page navigates when it opens. Without one it stays at
  // about:blank.
  std::optional<GURL> url;
  // When the navigation started: when the caller opened the Page, before its
  // process started, as Chrome's NavigationRequest starts a tab's navigation
  // before its renderer runs. What its document's timeOrigin counts from.
  base::TimeTicks navigation_start;
  // What a request of each Request Kind sends, and for a kind whose requests
  // can have a body, what one with a body sends, whose lines hold the
  // Content-Type where the Persona puts it, which the page's script gives, or
  // which goes if it gives none.
  std::map<PageRequestKind, PageRequestTemplate> requests;
  std::map<PageRequestKind, PageRequestTemplate> requests_with_body;
};

// Returns what mitto_page_open() reads from `json`, or nullopt if it holds an
// unknown key or an invalid value.
std::optional<PageOptions> ParsePageOptions(std::string_view json);

// How many requests a Page may have open at once, as many as Chrome's network
// service allows a renderer process. One that asks for more is refused.
// Upstream: services/network/network_context.h
inline constexpr size_t kMaxPageRequests = 2700;

// The keepalive requests in flight, and the bytes of their URLs and header
// lines, as the network service's KeepaliveStatisticsRecorder counts them, of
// a Session or of one of its Pages. Each request counts until it ends, after
// its Page has gone too.
// Upstream: services/network/keepalive_statistics_recorder.h
struct KeepaliveInFlight final : public base::RefCounted<KeepaliveInFlight> {
  KeepaliveInFlight();

  size_t requests = 0;
  size_t bytes = 0;

 private:
  friend class base::RefCounted<KeepaliveInFlight>;
  ~KeepaliveInFlight();
};

// How many keepalive requests may be in flight at once, of a Session and of
// one Page, which stands for its top-level frame, and how many bytes of URLs
// and header lines one Page's may hold. One past these fails with
// ERR_INSUFFICIENT_RESOURCES.
// Upstream: services/network/url_loader_factory.h
inline constexpr size_t kMaxKeepaliveRequests = 2048;
inline constexpr size_t kMaxKeepaliveRequestsPerPage = 256;
inline constexpr size_t kMaxKeepaliveBytesPerPage = 512 * 1024;

// What a Page's Session gives it, on the network thread: its requests and its
// cookies.
class PageNetwork {
 public:
  // Starts a request for a Page, which tells `client` how it goes until it
  // finishes or the client abandons it. It is the Session's, as a request of
  // the C API's is.
  virtual base::WeakPtr<RunningRequest> StartPageRequest(
      RequestClient& client,
      GURL url,
      net::HttpRequestHeaders headers,
      RequestOptions options,
      std::optional<std::vector<char>> body) = 0;

  virtual net::CookieStore& cookie_store() = 0;

  // The keepalive requests of the Session's Pages in flight.
  virtual const scoped_refptr<KeepaliveInFlight>& keepalive_in_flight() = 0;

  // The Session's network estimate and salts, or null for a Session that has
  // none.
  virtual const NetworkQuality* GetNetworkQuality() const = 0;

  // Hands a BroadcastChannel message of the Page `from`, whose document is of
  // `origin`, to each other Page of the Session, as Chrome's browser process
  // hands it to the renderers of other tabs.
  virtual void BroadcastFromPage(const PageHost& from,
                                 const url::Origin& origin,
                                 const PageBroadcast& broadcast) = 0;
  // The Session's Cache API, which its Pages share, and the quota of its
  // storage.
  virtual CacheStorageContext& cache_storage() = 0;
  virtual StorageQuota& storage_quota() = 0;

 protected:
  virtual ~PageNetwork() = default;
};

// How long a Page process has to end once its channel has closed, before it is
// killed. It ends at once when it reads the end of the stream, whatever its
// scripts are doing.
inline constexpr base::TimeDelta kPageExitGrace = base::Seconds(2);

// How many console messages and uncaught errors of a Page wait untaken at most,
// and how many bytes of their text, so that a Page that writes without end
// cannot fill the memory of the process that holds the library. V8's inspector
// keeps as many console messages for DevTools.
inline constexpr size_t kMaxWaitingReports = 1000;
inline constexpr size_t kMaxWaitingReportBytes = 32 * 1024 * 1024;

// One event of a Page. Each MITTO_PAGE_EVENT type uses the fields that
// MittoPageEvent says it does.
struct PageEvent {
  PageEvent();
  PageEvent(PageEvent&&);
  PageEvent& operator=(PageEvent&&);
  ~PageEvent();

  int32_t type = 0;
  int32_t error = 0;
  uint64_t call = 0;
  std::string json;
  std::string message;
  int32_t level = 0;
  int32_t status = 0;
  int32_t net_error = 0;
  std::string url;
};

// Carries a Page's events from the network thread to whichever thread holds the
// Page's handle. It outlives the Page's Session, so that a call made after the
// Session has gone still gets its event.
class PageEvents : public base::RefCountedThreadSafe<PageEvents> {
 public:
  PageEvents();

  PageEvents(const PageEvents&) = delete;
  PageEvents& operator=(const PageEvents&) = delete;

  // Called on any thread. Returns the number of a new call.
  uint64_t NewCall();

  // Called on any thread.
  //
  // Takes the next event into `event` and returns net::OK. Waits for one for up
  // to `timeout`, or for as long as it takes if that is nullopt. Returns
  // net::ERR_IO_PENDING if none arrived in that time, and at once after
  // EndWaits().
  int NextEvent(std::optional<base::TimeDelta> timeout, PageEvent& event);

  // Called on any thread. Ends each wait of NextEvent(), and makes each later
  // one return at once.
  void EndWaits();

  // The rest is called on the network thread.

  void Push(PageEvent event);

  // Pushes a console message or an uncaught error, unless kMaxWaitingReports of
  // them, or kMaxWaitingReportBytes of their text, wait untaken. Then it drops
  // it, and the next one that has room comes after a console warning that says
  // how many were dropped. Returns how many events it pushed.
  int PushReport(PageEvent event);

  // Pushes the MITTO_PAGE_EVENT_ENDED event, and remembers why, for the calls
  // that come later.
  void PushEnded(int32_t error, std::string message);

  // Pushes the failure of `call`, which came after the Page ended.
  void PushCallAfterEnd(uint64_t call);

 private:
  friend class base::RefCountedThreadSafe<PageEvents>;

  ~PageEvents();

  void AddReport(PageEvent event) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  base::Lock lock_;
  base::ConditionVariable event_waiting_{&lock_};
  base::circular_deque<PageEvent> events_ GUARDED_BY(lock_);
  uint64_t next_call_ GUARDED_BY(lock_) = 1;
  // Why the Page ended, once it has.
  int32_t ended_error_ GUARDED_BY(lock_) = 0;
  std::string ended_message_ GUARDED_BY(lock_);
  bool waits_ended_ GUARDED_BY(lock_) = false;
  // The console messages and uncaught errors among `events_`, and their text.
  size_t waiting_reports_ GUARDED_BY(lock_) = 0;
  size_t waiting_report_bytes_ GUARDED_BY(lock_) = 0;
  // How many were dropped since the last one that had room.
  uint64_t dropped_reports_ GUARDED_BY(lock_) = 0;
};

// One Page, on the network thread, where its Session owns it until the Page
// closes or the Session goes. Its events go into its PageEvents, and each lists
// the Page's handle in the Session's ReadyRequests, beside its requests.
//
// It serves what the Page asks of its Session: requests, which it starts from
// the template of their Request Kind with the Page's document as their
// Initiator, and the document's cookies, as Chrome's network service serves a
// renderer.
class PageHost final : public PageChannel::Delegate {
 public:
  PageHost(scoped_refptr<PageEvents> events,
           scoped_refptr<PageProcess> process,
           ReadyRequests& ready,
           uintptr_t handle,
           PageNetwork& network);

  PageHost(const PageHost&) = delete;
  PageHost& operator=(const PageHost&) = delete;

  // Abandons the Page's requests and closes the channel, which ends the Page
  // process. Whoever closes the Page reaps it.
  ~PageHost() override;

  // Starts to read `ours`, tells the Page how to start, and starts the
  // navigation, if `options` has a URL. Returns false if it cannot read
  // `ours`.
  bool Start(PageChannelEnd ours, PageOptions options);

  void Evaluate(uint64_t call, std::string script);

  // Ends each call in progress and the Page, because its Session is being
  // destroyed, and closes the channel.
  void EndWithSession();

  const scoped_refptr<PageProcess>& process() const { return process_; }

  // Sends the Page a BroadcastChannel message of another Page of the Session
  // whose document is of `origin`, if the Page's document is of that origin
  // too: the storage key of the top-level document, the only one a Page has.
  void DeliverBroadcast(const url::Origin& origin,
                        const PageBroadcast& broadcast);

  base::WeakPtr<PageHost> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  class ResponseChecks;
  class Request;

  // PageChannel::Delegate:
  void OnFrame(uint32_t type, std::string payload) override;
  void OnChannelClosed(PageChannel::Closed closed, std::string why) override;

  // A context of the Page's: a document, or a dedicated worker, by the ID
  // that the Page gave it.
  struct Context {
    // A document's URL, as the library loaded it after its navigation's
    // redirects, or about:blank; a worker's script's final URL, or its blob:
    // or data: URL.
    GURL url;
    // A document's origin, its URL's, or for a frame's initial about:blank its
    // parent's; a worker's, its creator's, or an opaque one for data:.
    url::Origin origin;
    // The document whose frame element holds a document's frame, or the
    // context that started a worker; kNoDocument for the top-level document.
    uint64_t parent = kNoDocument;
    bool is_worker = false;
  };

  // What a document's cookie calls are made for, as its frame's
  // RestrictedCookieManager has them: its URL, its frame's site for cookies,
  // and its partition.
  struct CookieContext {
    CookieContext();
    CookieContext(const CookieContext&);
    CookieContext& operator=(const CookieContext&);
    ~CookieContext();

    GURL url;
    net::SiteForCookies site_for_cookies;
    std::optional<net::CookiePartitionKey> partition_key;
  };

  // What the Page asks for. Each of these that comes from a context has what
  // the library loaded it as, never what the Page names.
  void OnRequest(PageRequest asked, uint64_t context_id);
  void OnCredit(const PageCredit& credit);
  void OnGetCookies(uint64_t question, const CookieContext& context);
  void OnSetCookie(const std::string& line, const CookieContext& context);
  void OnCancel(uint64_t number);
  // What the document's CookieStore asks, as Chrome's RestrictedCookieManager
  // answers it, always for the document that the Session loaded.
  void OnCookieStoreGet(PageCookieStoreGet get, const CookieContext& context);
  void OnCookieStoreSet(PageCookieStoreSet set, const CookieContext& context);
  void OnCookieStoreListen(bool listen, const CookieContext& context);
  // A message of one of the document's BroadcastChannels, for the Session's
  // other Pages of the document's origin, which the library takes from the
  // document it loaded, not from the Page.
  void OnBroadcast(const PageBroadcast& broadcast, const GURL& document_url);
  // A document that the Page has made, which takes the place of the
  // top-level one, or of its frame's document, or is a frame's first one.
  void OnDocumentCommitted(const PageDocumentCommitted& committed);
  // A dedicated worker that a context has started.
  void OnWorkerStarted(const PageWorkerStarted& started);
  // A context that goes while its parent stays.
  void OnContextGone(uint64_t context_id);
  // A navigation within document `document` that gave it the URL `url`.
  void OnSameDocumentNavigation(uint64_t document, const std::string& url);
  // Pushes MITTO_PAGE_EVENT_NAVIGATED for a navigation that ended at `url`
  // with `net_error`, and `status` for one that kept the document.
  void PushNavigated(std::string url, int32_t status, int32_t net_error);
  // Context `id`, or null after refusing a message of `type` that names a
  // context that is not the Page's.
  const Context* ContextOf(uint64_t id, uint32_t type);
  // Context `id` and each context under it: the documents of its frames, the
  // workers it started, and theirs.
  std::set<uint64_t> ContextsUnder(uint64_t id) const;
  // Forgets `ids`, which go, with what waits for them, and lets their
  // keepalive requests go on without them.
  void ForgetContexts(const std::set<uint64_t>& ids);
  // Lets each keepalive request of `ids`, or of every context for null, go on
  // without the Page, which hears that it ended.
  void LetKeepaliveRequestsGo(const std::set<uint64_t>* ids);
  // What a request of context `id` goes as, beside its Kind's: its
  // Initiator, its referrer's source, and its frame's place in the frame
  // tree. For a frame's navigation, `parent` is the frame's parent, from
  // which the tree counts.
  void FillContextFetch(uint64_t id, uint64_t parent, FetchParams& fetch) const;
  // The URL of the Page's top-level document, or an empty one before the Page
  // has made one.
  GURL TopDocumentUrl() const;
  // What the cookie calls of document `id` are made for.
  CookieContext CookieContextOf(uint64_t id) const;
  // Where document `id`'s frame is in the frame tree: the top-level
  // document's origin, and whether a document from `id` up is of another
  // site, or of another origin, than the top-level one.
  struct FrameTree {
    url::Origin top_frame_origin;
    bool is_top_level = true;
    bool cross_site = false;
    bool cross_origin = false;
  };
  FrameTree FrameTreeOf(uint64_t id) const;
  // What the document's Cache API asks, which PageCacheStorage serves,
  // with the storage key of the document that asks.
  void OnCacheStorage(PageCacheStorageCall call, const GURL& document_url);

  // What the library checks of a Page's request as it goes, each hop and
  // redirect included.
  struct FetchChecks {
    // Mode cors, a fetch's or an element's, whose responses from another
    // origin pass the CORS check.
    bool cors = false;
    // Mode same-origin: no hop may go to another origin.
    bool same_origin = false;
    // Mode no-cors: a response from another origin is opaque, and needs no
    // CORS check.
    bool no_cors = false;
    // Credentials mode include, whose CORS check wants the request's own
    // origin and Access-Control-Allow-Credentials.
    bool credentials = false;
    // A request that outlives its document and its Page.
    bool keepalive = false;
    // A navigation, whose response the Page makes a document of, unless it
    // is a 204, a 205 or a download.
    bool is_navigation = false;
  };

  // Starts a request numbered `number`, whose client is a Request that
  // `requests_` holds, and which goes on only as `checks` allow, for context
  // `context_id` of `document_origin`, opaque for a navigation.
  void StartRequest(uint64_t number,
                    uint64_t context_id,
                    const url::Origin& document_origin,
                    GURL url,
                    net::HttpRequestHeaders headers,
                    RequestOptions options,
                    std::optional<std::vector<char>> body,
                    FetchChecks checks);

  // What a Request hears of its job.
  void OnResponseStarted(Request& request, const RequestEvent& event);
  void OnBodyRead(Request& request, size_t size);
  // This deletes `request`.
  void OnRequestFinished(Request& request, int net_error);
  // Reads more of the body of `request` if the Page has given credit for it.
  void ReadIfCredited(Request& request);

  void SendCookies(uint64_t question,
                   const net::CookieAccessResultList& cookies,
                   const net::CookieAccessResultList& excluded);
  void SendCookieStoreCookies(const PageCookieStoreGet& get,
                              const net::CookieAccessResultList& cookies,
                              const net::CookieAccessResultList& excluded);
  void OnCookieStoreChange(const net::CookieChangeInfo& change);

  // Whether a document at `document_url` can have cookies, which only one that
  // the Session loaded over HTTP or HTTPS can.
  static bool DocumentHasCookies(const GURL& document_url);
  // The partition of the cookies of a frame of `frame_origin` under a
  // top-level frame of `top_frame_origin`, with `site_for_cookies`, as
  // Chrome's RestrictedCookieManager has it.
  static std::optional<net::CookiePartitionKey> FramePartitionKey(
      const url::Origin& top_frame_origin,
      const url::Origin& frame_origin,
      const net::SiteForCookies& site_for_cookies);
  // The storage key of a document at `document_url`, as
  // StorageKey::Serialize() writes it, if it may reach the Cache API: one of
  // an HTTP or HTTPS URL in a secure context. A top-level document's key is
  // its origin, first party.
  static std::optional<std::string> DocumentCacheStorageKey(
      const GURL& document_url);

  // What navigator.connection gives a document at `url`: what the renderer
  // process of its site gives for its host, as for a subresource to it.
  PageConnection ConnectionOf(const GURL& url) const;

  // Abandons each request in progress.
  void AbandonRequests();

  // Refuses what the Page sent: tells it why, closes the channel and kills the
  // process, because a Page that sends what it should not may be hostile.
  void Refuse(std::string why);

  // Closes the channel, and finds out how the process ended, without blocking
  // the network thread.
  void WaitForExit();
  void OnExited(std::string how);

  // Fails each call in progress, and then pushes MITTO_PAGE_EVENT_ENDED.
  void End(int32_t error, std::string message);

  void Push(PageEvent event);
  void PushReport(PageEvent event);

  const scoped_refptr<PageEvents> events_;
  const scoped_refptr<PageProcess> process_;
  const raw_ref<ReadyRequests> ready_;
  const uintptr_t handle_;
  const raw_ref<PageNetwork> network_;
  std::unique_ptr<PageChannel> channel_;
  PageOptions options_;
  // The Page's contexts, its documents and dedicated workers, by the ID that
  // the Page gave each: the Initiator of their requests, and whose cookies a
  // document's document.cookie reads and writes. A Page has one top-level
  // document once it has told the library of it, and the documents of its
  // frames and its workers under it.
  std::map<uint64_t, Context> contexts_;
  // The largest ID the Page has given a context.
  uint64_t last_context_id_ = kNoDocument;
  // The navigations of frames whose responses the Page has and has not made a
  // document of yet, by number: the final URL, the frame's parent and its
  // document that the new one replaces.
  struct FrameNavigationResponse {
    GURL url;
    uint64_t parent = kNoDocument;
    uint64_t target = kNoDocument;
  };
  std::map<uint64_t, FrameNavigationResponse> frame_navigation_responses_;
  // The responses of the scripts of workers that the Page has and has not
  // started yet, by number: the context that asked and the final URL.
  struct WorkerScript {
    uint64_t creator = kNoDocument;
    GURL url;
  };
  std::map<uint64_t, WorkerScript> worker_scripts_;
  // The navigations whose responses the Page has and has not made a document
  // of yet, by number: the final URL and the status.
  struct NavigationResponse {
    GURL url;
    int32_t status = 0;
  };
  std::map<uint64_t, NavigationResponse> navigation_responses_;
  // While a document's CookieStore listens: what tells it of each change, and
  // what the document's cookie calls are made for.
  std::unique_ptr<net::CookieChangeSubscription> cookie_store_subscription_;
  CookieContext cookie_store_context_;
  // The requests in progress, by number, and the largest number the Page has
  // used.
  std::map<uint64_t, std::unique_ptr<Request>> requests_;
  uint64_t last_request_number_ = kNavigationRequest;
  // The Page's keepalive requests in flight, with those that have outlived
  // their documents.
  const scoped_refptr<KeepaliveInFlight> keepalive_in_flight_ =
      base::MakeRefCounted<KeepaliveInFlight>();
  bool hello_received_ = false;
  // Why the Page refused what the library sent, if it did.
  std::optional<std::string> refused_by_page_;
  // Why the library refused what the Page sent, if it did.
  std::optional<std::string> refused_by_library_;
  bool waiting_for_exit_ = false;
  bool ended_ = false;
  // The calls in progress, which the Page answers in any order.
  std::set<uint64_t> calls_;
  // The document's Cache API, from its first call.
  std::unique_ptr<PageCacheStorage> cache_storage_;
  // The first number that the next document's Cache API gives a cache or a
  // stored body, past those of the documents before.
  uint64_t next_cache_storage_number_ = 1;
  base::WeakPtrFactory<PageHost> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_HOST_H_
