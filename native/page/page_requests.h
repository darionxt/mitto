// A Page's requests to its Session, on the Page program's main thread. It
// numbers them, hands each response to whoever asked for it, and gives the
// Session credit for a body only as its reader takes it, so that a reader that
// waits, such as a parser that waits for a script, slows the server down as a
// slow reader of a request does.

#ifndef MITTO_PAGE_PAGE_REQUESTS_H_
#define MITTO_PAGE_PAGE_REQUESTS_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "mitto/page/page_reporter.h"
#include "mitto/page/session_sender.h"
#include "mitto/page_messages.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-persistent-handle.h"

namespace mitto {

class ExecutionContext;

class PageRequests {
 public:
  // Hears how one request goes.
  class Client {
   public:
    // The response, after any redirects.
    virtual void OnResponseStarted(const PageResponseStarted& response) = 0;
    // The next bytes of the body. The client calls TookBody() once it has
    // taken them, now or later, which gives the Session room for as many
    // more.
    virtual void OnResponseBody(std::string bytes) = 0;
    // What the request took, which the Session says at its end, just before
    // OnResponseEnded().
    virtual void OnResponseTotals(const PageResponseEnded& ended) {}
    // The end: net::OK when all of the body has come, and otherwise why the
    // request failed, with or without a response. Nothing more comes.
    virtual void OnResponseEnded(int net_error) = 0;
    // For a request whose body streams: the Session has given credit for
    // more of it.
    virtual void OnRequestBodyCredit() {}
    // For a navigation: whether the document of its response is cross-origin
    // isolated, and its embedder policy, which the Session says just before
    // the response starts.
    virtual void OnIsolation(const PageIsolation& isolation) {}
    // For a traversal to the entry of a document that the back/forward cache
    // keeps: the Session restores that document, in place of a response.
    // Nothing more comes.
    virtual void OnRestore() {}

   protected:
    virtual ~Client() = default;
  };

  // `sender` sends frames to the Session, and `refuse` refuses what it sent,
  // saying why. The responses of a dedicated worker's requests reach its
  // thread through `route`; without one they reach the main thread.
  PageRequests(scoped_refptr<SessionSender> sender,
               base::RepeatingCallback<void(std::string why)> refuse,
               SessionSender::ResponseRoute route = {});

  PageRequests(const PageRequests&) = delete;
  PageRequests& operator=(const PageRequests&) = delete;

  ~PageRequests();

  // Has `client` hear of the navigation that the Session makes when the Page
  // opens, if it makes one: its response is the Page's document.
  void SetNavigationClient(Client& client);

  // Asks the Session for a request of `kind` for `url`, from the current
  // context, a document or a worker, from which the Session works out its
  // Initiator. `client` hears how it goes, and has to live until its end. The
  // current window's Performance Timeline gets an entry of it once it ends, as
  // Blink's ResourceFetcher adds one, which says that it blocked rendering if
  // `render_blocking`.
  uint64_t Start(PageRequestKind kind,
                 std::string url,
                 Client& client,
                 bool render_blocking = false);
  // The same for `request`, whose number this sets, with the entry's
  // initiator type, or the kind's own if it is empty: "script" for a script,
  // a module or a module of a worker's top-level graph, and "fetch" for
  // anything else.
  uint64_t Start(PageRequest request,
                 Client& client,
                 bool render_blocking = false,
                 std::u16string initiator_type = std::u16string());

  // The same for a dedicated worker's script, which Chrome's browser fetches
  // and its WorkerMainScriptLoader times in the creator's timeline as
  // "other": from when the response started, with none of the request's own
  // steps, nor its redirects, and only for an ok response that ends whole.
  uint64_t StartWorkerScript(PageRequest request, Client& client);
  // The same for a synchronous request, as importScripts() makes one: this
  // thread waits for each part of its response and hands it to `client`,
  // which takes each part of the body as it comes, and returns once it has
  // ended, or once `abort` is signaled, as when the worker ends, which cancels
  // it.
  void StartSynchronous(PageRequest request,
                        Client& client,
                        base::WaitableEvent& abort);

  // Asks the Session for `request`, a navigation that document `initiator`
  // starts, whose number this sets, and returns it. Its response is a
  // document's, which gets no entry in the Performance Timeline of the window
  // that asked.
  uint64_t StartNavigation(uint64_t initiator,
                           PageRequest request,
                           Client& client);
  // The same for a traversal of the session history to an entry of another
  // document, which the Session makes from what it kept of the navigation it
  // repeats.
  uint64_t StartTraversal(PageTraversalRequest traversal, Client& client);

  // Tells the Session that request `number` is no longer wanted. Its client
  // hears nothing more of it.
  void Cancel(uint64_t number);
  // The same for every request that has a client, as when a worker ends.
  void CancelAll();

  // Lets keepalive request `number` go on without its client, which hears
  // nothing more of it, and without its document, which is going and adds no
  // entry of it. The Session lets it outlive the document, and ends it for
  // the Page, once it hears of the next document.
  void LetGo(uint64_t number);
  // Has the Session send deferred request `number` now, or forgets one whose
  // document is going, which the Session sends then, and of which neither
  // ends for the Page.
  void SendDeferred(uint64_t number);
  void ForgetDeferred(uint64_t number);

  // Sends the Session a network hint of the current window's document.
  void Hint(const PageNetworkHint& hint);
  // Tells the Session that document `document`'s window has dispatched its
  // load event, unless the Session has forgotten that document, as a frame's
  // whose element a load listener removed.
  void SendLoaded(uint64_t document);

  // The client of request `number` has taken `bytes` more of its body.
  void TookBody(uint64_t number, size_t bytes);

  // For request `number`, whose body streams: how many more bytes of the body
  // the Session has given credit for, none until it starts reading it; sends
  // `bytes` of it, at most that many; and ends it, which fails the request if
  // `errored`. Once the request has ended these do nothing.
  size_t BodyCredit(uint64_t number) const;
  void SendBodyPart(uint64_t number, std::string_view bytes);
  void EndBody(uint64_t number, bool errored);

  // What the Session sends of the requests of a window that the back/forward
  // cache keeps frozen (ExecutionContext::IsInBackForwardCache()) waits, in
  // the order it came, as Blink's loaders buffer what comes for a frozen page
  // (LoaderFreezeMode::kBufferIncoming), and the bodies' connections go on
  // meanwhile. The window's page goes once the bodies that wait are more than
  // BackForwardCacheBufferLimitTracker lets a process buffer.
  //
  // The requests of `context` that something waits for, the oldest first.
  std::vector<uint64_t> HeldRequestsOf(const ExecutionContext& context) const;
  // Hands over what waits for request `number`, in the order it came, unless
  // its window is frozen, as the task in which Blink's loader flushes what it
  // deferred.
  void DeliverHeld(uint64_t number);
  // Whether a request of `context` is still open.
  bool IsLoading(const ExecutionContext& context) const;
  // Cancels each request of `context`, as when the back/forward cache drops
  // its window, which hears nothing more of them.
  void CancelAllOf(const ExecutionContext& context);

  // What the Session sends.
  void OnResponseStarted(const PageResponseStarted& response);
  void OnResponseBody(PageResponseBody body);
  void OnResponseEnded(const PageResponseEnded& ended);
  void OnRequestBodyCredit(const PageCredit& credit);
  void OnIsolation(const PageIsolation& isolation);
  void OnRestore(uint64_t number);

 private:
  // What the entry of a request of the Page's is made of: ResourceFetcher's
  // PendingResourceTimingInfo, with the response and the size of its body.
  struct Timed {
    Timed();
    Timed(Timed&&);
    Timed& operator=(Timed&&);
    ~Timed();

    base::TimeTicks start_time;
    std::u16string initiator_type;
    std::u16string url;
    bool render_blocking = false;
    // Whether the Session follows its redirects.
    bool follows_redirects = true;
    // Whether it is a module of a module worker's top-level graph, a request
    // of Kind worker in mode cors, whose entry goes to the creator's
    // timeline.
    bool outside_settings = false;
    // Whether it is a dedicated worker's script (StartWorkerScript()).
    bool worker_script = false;
    std::optional<PageResponseStarted> response;
    uint64_t decoded_body_size = 0;
    // The window or worker that asked, whose timeline gets the entry while it
    // runs, and its context, in which the entry is added, whoever ends the
    // request.
    cppgc::WeakPersistent<ExecutionContext> execution_context;
    v8::Global<v8::Context> context;
  };

  struct Open {
    Open();
    Open(Open&&);
    Open& operator=(Open&&);
    ~Open();

    raw_ptr<Client> client;
    // Bytes the Session has sent that the client has not taken yet, and that
    // it has no credit for: at most kBodyWindow.
    size_t untaken = 0;
    // Those that came while its window was frozen and gave the Session credit
    // then.
    size_t credited = 0;
    bool started = false;
    // For a body that streams: how much of it the Session has given credit
    // for, which it does first when it starts reading it, and how much the
    // Page has sent.
    bool streams_body = false;
    bool body_ended = false;
    uint64_t body_credited = 0;
    uint64_t body_sent = 0;
    // For a request of the Page's, not the navigation.
    std::optional<Timed> timed;
    // A navigation, the Session's or a document's, and whether the Session
    // has said whether its document is cross-origin isolated.
    bool navigation = false;
    bool isolation_said = false;
    // Whether its response counts as a script's request's here: the favicon.
    bool script_request = false;
    // Whether it is a traversal that the Session may answer with a restore.
    bool may_restore = false;
  };

  // What the Session sent of a request of a frozen window.
  using Held = std::variant<PageResponseStarted,
                            PageResponseBody,
                            PageResponseEnded,
                            PageCredit>;
  // Whether what the Session sends of request `number` waits: its window is
  // frozen, or something of it waits already.
  bool ShouldHold(uint64_t number) const;
  void HoldMessage(uint64_t number, Held message);
  // Holds a part of a body, whose bytes give the Session credit now, while the
  // bodies that wait are within the limit.
  void HoldBody(PageResponseBody body);
  void SendCredit(uint64_t number, size_t bytes);
  // Whether `open` is a request of a window that the back/forward cache keeps
  // frozen.
  static bool IsFrozen(const Open& open);
  // What the On...() methods do with what they do not hold.
  void ProcessResponseStarted(const PageResponseStarted& response);
  // Whose bytes were counted as it was held, if `held`.
  void ProcessResponseBody(PageResponseBody body, bool held);
  void ProcessResponseEnded(const PageResponseEnded& ended);
  void ProcessRequestBodyCredit(const PageCredit& credit);

  // Returns the request that the Session names, or null after refusing what
  // it sent. The navigation that the Session makes when the Page opens is
  // found once the Session sends something of it.
  Open* Find(uint64_t number, std::string_view message);
  // Numbers `request` and sends it as `context`'s, with its response going
  // to `route`, and returns its number.
  uint64_t Send(uint64_t context,
                PageRequest& request,
                const SessionSender::ResponseRoute& route);
  uint64_t Send(uint64_t context, PageRequest& request);
  // Adds the entry of a request that ended at `response_end`, with `ended`'s
  // totals if the Session said them.
  static void AddResourceTiming(Timed& timed,
                                base::TimeTicks response_end,
                                const PageResponseEnded* ended);
  // Whether a worker's script loaded, as Chrome's WorkerScriptFetcher hands
  // the renderer only an ok response that came whole.
  static bool WorkerScriptLoaded(const Timed& timed,
                                 const PageResponseEnded& ended);

  const scoped_refptr<SessionSender> sender_;
  const base::RepeatingCallback<void(std::string)> refuse_;
  const SessionSender::ResponseRoute route_;
  std::map<uint64_t, Open> open_;
  // What waits of each request, in the order it came, and the bytes of the
  // bodies among it.
  std::map<uint64_t, std::deque<Held>> held_;
  size_t held_bytes_ = 0;
  raw_ptr<Client> navigation_client_ = nullptr;
  bool navigation_seen_ = false;
};

}  // namespace mitto

#endif  // MITTO_PAGE_PAGE_REQUESTS_H_
