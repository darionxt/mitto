// The messages between a Page and its Session, and the frames that carry them
// over the channel between their two processes. The library and the Page
// program both build this file. The Session reads what a Page sends as hostile
// input, so each frame from a Page is checked here, field by field, before the
// library acts on it.
//
// docs/runtime.md describes the channel and each message.

#ifndef MITTO_PAGE_MESSAGES_H_
#define MITTO_PAGE_MESSAGES_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/types/expected.h"

namespace base {
class DictValue;
}  // namespace base

namespace mitto {

// Both ends speak exactly this version and refuse any other, because the Page
// program ships beside the library, built from the same tree. Increase it with
// any change to the messages below.
inline constexpr uint32_t kPageProtocolVersion = 14;

// A frame is the size of its payload and its type, each a 32-bit little-endian
// number, and then the payload.
inline constexpr size_t kFrameHeaderSize = 8;

// Each end refuses a frame with a larger payload, so that the other cannot make
// it hold more than this for one message.
inline constexpr size_t kMaxPayloadSize = 16 * 1024 * 1024;

// The longest script that one kEvaluate carries.
inline constexpr size_t kMaxScriptSize = kMaxPayloadSize - 8;

// The longest result or reason that one kEvaluated carries.
inline constexpr size_t kMaxEvaluatedTextSize = kMaxPayloadSize - 9;

// The longest text that one kConsole carries.
inline constexpr size_t kMaxConsoleTextSize = kMaxPayloadSize - 1;

// The number of the request that the Session makes when the Page opens: its
// navigation, whose response is the Page's document. The Page numbers its own
// requests from 1 up.
inline constexpr uint64_t kNavigationRequest = 0;

// The net error of a kResponseEnded whose request has all of its body:
// net::OK, which the Page program, with no //net, cannot name.
inline constexpr int32_t kResponseComplete = 0;

// How many bytes of a response's body the Session sends before the Page gives
// credit for them, as much as RequestChannel holds for a caller. Each request
// starts with this much.
inline constexpr uint32_t kBodyWindow = 64 * 1024;

// The argument that starts the Page program for a Session:
// "--session=CHANNEL,PARENT", where CHANNEL is the descriptor of its end of the
// channel on macOS and Linux and the value of its handle on Windows, and PARENT
// is the process ID of the process that holds the library.
inline constexpr std::string_view kSessionArgumentPrefix = "--session=";

// How the Page program ends, beside a crash or a kill. 0 says that the channel
// closed.
//
// The Page refused a message, or could not start.
inline constexpr int kPageExitRefused = 1;
// V8 ran out of memory.
inline constexpr int kPageExitOutOfMemory = 3;

// The types of message. Each says which way it goes and what its payload holds.
enum class PageMessage : uint32_t {
  // Each way, first and only once: the version of these messages that the
  // sender speaks, as a 32-bit number.
  kHello = 1,
  // Each way: why the sender refuses what it got, in UTF-8. The sender closes
  // the channel after it.
  kRefused = 2,
  // To the Page, once, right after kHello: what the Page starts with, as the
  // JSON object that EncodeStart() writes.
  kStart = 3,
  // To the Page: the number of a call, as a 64-bit number, and then a script,
  // in UTF-8.
  kEvaluate = 4,
  // To the Session: the number of the call, a byte that is 0 if the script has
  // a result and 1 if it failed, and then the result as JSON, or why it
  // failed, in UTF-8.
  kEvaluated = 5,
  // To the Session: a byte that holds a ConsoleLevel, and then the text of a
  // message that a script wrote to the console, in UTF-8.
  kConsole = 6,
  // To the Session: an error that no script caught, such as a promise rejected
  // with no handler, in UTF-8.
  kUncaughtError = 7,
  // To the Session: a request of the Page's, as PageRequest says. Its number
  // is larger than that of each request before it.
  kRequest = 8,
  // To the Session: the number of a request, as a 64-bit number, and how many
  // more bytes of its body the Page has taken, as a 32-bit number. The Session
  // may send that many more.
  kCredit = 9,
  // To the Session: the number of a question for the document's cookies, as a
  // 64-bit number. kCookies answers it.
  kGetCookies = 10,
  // To the Session: what a script set document.cookie to, in UTF-8.
  kSetCookie = 11,
  // To the Session, with no payload: the window's load event has been
  // dispatched.
  kLoaded = 12,
  // To the Page: the response of a request, as PageResponseStarted says.
  kResponseStarted = 13,
  // To the Page: the number of a request, as a 64-bit number, and the next
  // bytes of its body, at most as many as the Page has given credit for.
  kResponseBody = 14,
  // To the Page: the number of a request, as a 64-bit number, and a net error,
  // as a 32-bit number: 0 when all of the body has come, and otherwise why the
  // request failed, with or without a response.
  kResponseEnded = 15,
  // To the Page: the number of a kGetCookies, as a 64-bit number, and the
  // document's cookies as a Cookie header's line holds them.
  kCookies = 16,
  // To the Session: the number of a request, as a 64-bit number, which the
  // Page no longer wants. Its end follows, with net::ERR_ABORTED unless it
  // has ended already.
  kCancel = 17,
  // To the Session: the name of a member whose behaviour the Runtime lacks, a
  // Known Gap, such as "URL.prototype.href", in UTF-8, the first time the
  // document reaches it.
  kKnownGap = 18,
  // To the Session: a question of the document's CookieStore, as
  // PageCookieStoreGet says. kCookieStoreCookies answers it.
  kCookieStoreGet = 19,
  // To the Session: a cookie for the document's CookieStore to store, as
  // PageCookieStoreSet says. kCookieStoreStored answers it.
  kCookieStoreSet = 20,
  // To the Session: a byte, 1 for the document's CookieStore to hear of each
  // change to the document's cookies, and 0 to stop.
  kCookieStoreListen = 21,
  // To the Page: the cookies that a kCookieStoreGet asked for, as
  // PageCookieStoreCookies says.
  kCookieStoreCookies = 22,
  // To the Page: the number of a kCookieStoreSet, as a 64-bit number, and a
  // byte, 1 if the cookie was stored and 0 if not.
  kCookieStoreStored = 23,
  // To the Page, while the CookieStore listens: a change to the document's
  // cookies, as PageCookieStoreChange says.
  kCookieStoreChange = 24,
  // To the Session: a message of one of the document's BroadcastChannels, as
  // PageBroadcast says, for the Session's other Pages whose documents are of
  // the same origin.
  kBroadcast = 25,
  // To the Page: a message of a BroadcastChannel of another Page of the
  // Session whose document is of the same origin, as PageBroadcast says.
  kBroadcastReceived = 26,
};

// The Request Kinds that a Page asks its Session for, beside the navigation
// that opens it.
enum class PageRequestKind : uint8_t {
  kScript = 1,
  kFetch = 2,
};

// The level of a console message, as the console method that wrote it says.
enum class ConsoleLevel : uint8_t {
  kDebug = 1,
  kLog = 2,
  kInfo = 3,
  kWarning = 4,
  kError = 5,
};

// A message that a script wrote to the console.
struct PageConsoleMessage {
  PageConsoleMessage();
  PageConsoleMessage(PageConsoleMessage&&);
  PageConsoleMessage& operator=(PageConsoleMessage&&);
  ~PageConsoleMessage();

  ConsoleLevel level = ConsoleLevel::kLog;
  std::string text;
};

// Whether Chrome's window fills the work area.
enum class WindowState : uint8_t {
  kMaximised = 1,
  kNormal = 2,
};

// A brand of the browser and its version, as sec-ch-ua lists them.
struct PageBrandVersion {
  std::string brand;
  std::string version;

  friend bool operator==(const PageBrandVersion&,
                         const PageBrandVersion&) = default;
};

// What the browser tells each renderer of its user agent, which
// navigator.userAgentData gives a page: blink::UserAgentMetadata, from which
// the user agent client hints of the Session's requests come too.
struct PageUserAgentData {
  PageUserAgentData();
  PageUserAgentData(const PageUserAgentData&);
  PageUserAgentData& operator=(const PageUserAgentData&);
  ~PageUserAgentData();

  // In the order of sec-ch-ua.
  std::vector<PageBrandVersion> brands;
  // In the order of sec-ch-ua-full-version-list.
  std::vector<PageBrandVersion> full_version_list;
  bool mobile = false;
  // Such as "Windows".
  std::string platform;
  // Such as "19.0.0": the Machine's.
  std::string platform_version;
  std::string architecture;
  std::string bitness;
  std::string model;
  std::string ua_full_version;
  bool wow64 = false;
  std::vector<std::string> form_factors;
};

// What the browser tells each renderer of itself, which navigator reads.
struct PageNavigator {
  PageNavigator();
  PageNavigator(const PageNavigator&);
  PageNavigator& operator=(const PageNavigator&);
  ~PageNavigator();

  // The User-Agent of the Session.
  std::string user_agent;
  // What navigator.platform gives, such as "Win32".
  std::string platform;
  // The languages of Chrome's setting, separated by commas, such as
  // "en-US,en", which the Session's Accept-Language comes from.
  std::string languages;
  PageUserAgentData user_agent_data;
};

// What navigator.connection gives a document: the Session's network estimate,
// with the noise that the renderer process of the document's site adds for
// its host, capped and rounded. It is what a fetch() from that document to its
// own host sends in rtt and downlink.
struct PageConnection {
  // A multiple of 50 milliseconds, at most 3000.
  uint32_t rtt = 0;
  // In Mbps, a multiple of 0.05, at most 10.
  double downlink = 0;
};

// The kinds of media device that the Machine has, which
// navigator.mediaDevices.enumerateDevices() lists one entry of each for.
struct PageMediaDevices {
  bool audio_input = false;
  bool video_input = false;
  bool audio_output = false;
};

// How the last hop of a request went, as //net's LoadTimingInfo has it and
// Chrome's network service hands it to Blink for the Performance Timeline:
// each a time of base::TimeTicks in microseconds, which every process of the
// machine shares, and 0 for a step that did not happen.
struct PageLoadTiming {
  // When the hop started: the request's start, or when it followed its last
  // redirect.
  int64_t request_start = 0;
  int64_t domain_lookup_start = 0;
  int64_t domain_lookup_end = 0;
  // From the start of the connection, its DNS lookup included, to the end of
  // its TLS handshake.
  int64_t connect_start = 0;
  int64_t connect_end = 0;
  int64_t ssl_start = 0;
  int64_t send_start = 0;
  // The first and last byte of the headers, an informational response's
  // included, and the first byte of the final response's headers.
  int64_t receive_headers_start = 0;
  int64_t receive_headers_end = 0;
  int64_t receive_non_informational_headers_start = 0;
  // Whether the hop reused a connection.
  bool socket_reused = false;
};

// What the variations seed of a Page's Chrome Release says of
// kDeprecateUnload, which takes "unload" out of the permissions policy of the
// pages it covers.
struct DeprecatedUnload {
  // kDeprecateUnloadByBucket's parameters: the share of hosts covered, which
  // hosts those are for this installation.
  struct ByBucket {
    int percent = 100;
    // None where the seed gives each installation a bucket of its own, which
    // only a percent between 0 and 100 needs: then no host is in the share.
    std::optional<int> bucket;
  };

  DeprecatedUnload();
  DeprecatedUnload(const DeprecatedUnload&);
  DeprecatedUnload& operator=(const DeprecatedUnload&);
  ~DeprecatedUnload();

  // Whether kDeprecateUnload is on.
  bool enabled = false;
  // The hosts of kDeprecateUnloadByAllowList's "allowlist" when that feature
  // is on, where an empty list covers every host.
  std::optional<std::vector<std::string>> allow_list;
  // kDeprecateUnloadByBucket's parameters when that feature is on.
  std::optional<ByBucket> by_bucket;
};

// What a Page turns on and off beside the Engine Release's runtime-enabled
// features and V8's defaults, as its Persona says: what its Chrome Release did
// not ship yet, and what the variations seed of that release switches.
struct PageFeatures {
  PageFeatures();
  PageFeatures(const PageFeatures&);
  PageFeatures& operator=(const PageFeatures&);
  ~PageFeatures();

  // Names of runtime-enabled features to turn on, and then off, as Chrome's
  // --enable-blink-features and --disable-blink-features take them. Which
  // features exist only the Page knows, because only it holds the bindings.
  std::vector<std::string> enable_blink_features;
  std::vector<std::string> disable_blink_features;
  // V8's flags, such as "--no-foo" or "--foo=1", in the order to set them,
  // after those of the Engine Release.
  std::vector<std::string> v8_flags;
  DeprecatedUnload deprecated_unload;
};

// What a Page starts with, which it would otherwise read from the machine and
// the browser: what its Session's Machine, Persona and settings say. Places and
// sizes on the desktop are in device-independent pixels, from the display's
// top left corner.
struct PageStart {
  PageStart();
  PageStart(const PageStart&);
  PageStart& operator=(const PageStart&);
  ~PageStart();

  // The IANA name of the time zone that ICU, and so V8, reports, as Chrome on
  // Windows names it.
  std::string time_zone;
  // The BCP 47 tag of ICU's default locale, which Intl falls back to: the
  // Persona's.
  std::string locale;
  // The number of logical processors, which navigator.hardwareConcurrency
  // gives.
  int processors = 0;
  // How many touch points the touch screen takes, or 0 without one, which
  // navigator.maxTouchPoints gives.
  int touch_points = 0;
  // The display less the taskbar.
  int work_area_x = 0;
  int work_area_y = 0;
  int work_area_width = 0;
  int work_area_height = 0;
  WindowState window_state = WindowState::kMaximised;
  // Chrome's window, with its bars and borders.
  int window_x = 0;
  int window_y = 0;
  int window_width = 0;
  int window_height = 0;
  // Windows' contrast theme, such as "aquatic", or "none".
  std::string contrast_theme;
  // Whether Windows' colour filter inverts colours.
  bool inverted_colors = false;
  // The memory in GiB as Chrome rounds it, 2, 4, 8, 16 or 32, which
  // navigator.deviceMemory gives, and from which V8 works out its heap's
  // limits, as gin works them out from the machine's.
  int memory = 0;
  // The primary display.
  int screen_width = 0;
  int screen_height = 0;
  // The display's scale, such as 1.25, which devicePixelRatio gives.
  double scale = 1;
  // The window's page area.
  int viewport_width = 0;
  int viewport_height = 0;
  // "light" or "dark".
  std::string color_scheme;
  // "no-preference" or "reduce".
  std::string reduced_motion;
  std::string reduced_transparency;
  // The effective connection type, such as "4g", as the ect hint says.
  std::string effective_type;
  PageNavigator navigator;
  // What about:blank's document reads, which the library works out and adds
  // to what the "start" option of mitto_page_open() holds.
  PageConnection connection;
  // What the Persona's Chrome Release turns on and off.
  PageFeatures features;
  PageMediaDevices media_devices;
};

// What a call to a Page gave back.
struct PageEvaluated {
  PageEvaluated();
  PageEvaluated(PageEvaluated&&);
  PageEvaluated& operator=(PageEvaluated&&);
  ~PageEvaluated();

  uint64_t call = 0;
  // Whether the script failed, and `text` says why. Otherwise `text` is the
  // result as JSON, empty if JSON.stringify() writes nothing for it.
  bool failed = false;
  std::string text;
};

// A request that a Page asks its Session for. Its Initiator is the Page's
// document, as the Session loaded it, so the Page does not say it.
//
// Its payload is the number, as a 64-bit number, the Kind, as a byte, a byte
// that is 1 for a fetch of mode same-origin, the URL, the method and the
// header lines, each after its size as a 32-bit number, and then a byte that is
// 1 if a body follows, which is the rest.
struct PageRequest {
  PageRequest();
  PageRequest(PageRequest&&);
  PageRequest& operator=(PageRequest&&);
  ~PageRequest();

  uint64_t number = 0;
  PageRequestKind kind = PageRequestKind::kScript;
  // In UTF-8, as the Page resolved it. The Session checks that it is a URL.
  std::string url;
  // A token that is not a forbidden method: GET, and no lines and no body, for
  // a script.
  std::string method = "GET";
  // What the page's script set, in its order, each "name: value" and a line
  // feed, with a token for a name and no forbidden request header among them.
  std::string headers;
  // None with GET or HEAD.
  std::optional<std::string> body;
  // A fetch of mode same-origin, which no hop may take to another origin.
  bool same_origin = false;
};

// How much more of a request's body a Page has taken.
struct PageCredit {
  uint64_t number = 0;
  uint32_t bytes = 0;
};

// The response of a request, after any redirects, which the Session follows
// as a request of its own does.
struct PageResponseStarted {
  PageResponseStarted();
  PageResponseStarted(const PageResponseStarted&);
  PageResponseStarted& operator=(const PageResponseStarted&);
  PageResponseStarted(PageResponseStarted&&);
  PageResponseStarted& operator=(PageResponseStarted&&);
  ~PageResponseStarted();

  uint64_t number = 0;
  int32_t status = 0;
  // Whether the request followed a redirect to get here.
  bool redirected = false;
  // Whether the response's tainting is cors: a hop of the request went to
  // another origin than the document's, so the page's scripts read only the
  // header names that CORS lets them.
  bool cors = false;
  // For the navigation, when it started, as base::TimeTicks holds it, which
  // every process of the machine shares: what the document's
  // performance.timeOrigin counts from. 0 for the Page's own requests.
  int64_t navigation_start = 0;
  // Of the connection, as net::HttpResponseInfo has it: whether it went over
  // HTTP/2 or QUIC, and whether ALPN chose a protocol.
  bool was_fetched_via_spdy = false;
  bool was_alpn_negotiated = false;
  // For the navigation, what the document's navigator.connection gives. 0 for
  // the Page's own requests.
  PageConnection connection;
  // The URL of the response, in UTF-8.
  std::string url;
  // The status line's reason phrase, in bytes.
  std::string status_text;
  // What net::HttpConnectionInfoToString() calls the connection's protocol,
  // such as "h2", and the protocol that ALPN chose, "unknown" for none, each
  // a name of at most 64 characters.
  std::string connection_info;
  std::string alpn_negotiated_protocol;
  // The header lines, each "name: value" and a line feed, as the C API's
  // events hold them, but for Set-Cookie and Set-Cookie2.
  std::string headers;
  // For the Performance Timeline, as Chrome's network service tells Blink:
  // how the last hop went;
  PageLoadTiming timing;
  // whether the response passed the Fetch Standard's TAO check on each hop,
  // and whether its tainting is opaque, as a script's is once a hop has gone
  // to another origin than the document's;
  bool timing_allow_passed = false;
  bool opaque = false;
  // whether the cache had the server validate it, and how many bytes had
  // come by its start, headers included, none for one from the cache;
  bool validated = false;
  int64_t received_bytes = 0;
  // how many redirects it followed, at most 20, and whether one went to
  // another origin than the URL it came from;
  uint32_t redirect_count = 0;
  bool cross_origin_redirect = false;
  // for the navigation, whether it restarted for Critical-CH, which Chrome
  // makes a redirect to where it started;
  bool restarted_for_critical_ch = false;
  // when its first hop started, when the last redirect's headers ended, 0
  // without one, and when the navigation restarted for Critical-CH, if no
  // redirect came after.
  int64_t first_request_start = 0;
  int64_t last_redirect_end = 0;
  int64_t critical_ch_restart = 0;
};

// Bytes of a response's body.
struct PageResponseBody {
  PageResponseBody();
  PageResponseBody(PageResponseBody&&);
  PageResponseBody& operator=(PageResponseBody&&);
  ~PageResponseBody();

  uint64_t number = 0;
  std::string bytes;
};

// The end of a request.
struct PageResponseEnded {
  uint64_t number = 0;
  // net::OK when all of the body has come.
  int32_t net_error = 0;
  // What it took, as Chrome's network service tells Blink at its completion:
  // the body's bytes as they came over the wire, before any content coding
  // was undone, every byte received, headers included, and when it ended, a
  // time of base::TimeTicks in microseconds, 0 for a request that the library
  // refused before it started, as Blink fails such a fetch before it loads.
  int64_t encoded_body_size = 0;
  int64_t received_bytes = 0;
  int64_t completion_time = 0;
};

// CookieSameSite's values, and none for a cookie whose same-site mode is
// unspecified.
enum class PageCookieSameSite : uint8_t {
  kNone = 0,
  kStrict = 1,
  kLax = 2,
  kNoRestriction = 3,
};

// A cookie as the CookieStore hands it to a page, a CookieListItem: the
// cookie's own fields, its same-site mode as its access says, and when it
// expires, in microseconds since the Unix epoch, for one that does.
struct PageCookie {
  PageCookie();
  PageCookie(const PageCookie&);
  PageCookie& operator=(const PageCookie&);
  PageCookie(PageCookie&&);
  PageCookie& operator=(PageCookie&&);
  ~PageCookie();

  std::string name;
  std::string value;
  // As the cookie holds it: with a leading dot for a domain cookie.
  std::string domain;
  std::string path;
  std::optional<int64_t> expires;
  bool secure = false;
  PageCookieSameSite same_site = PageCookieSameSite::kNone;
  bool partitioned = false;
};

// A question of a CookieStore: the cookies of the document named `name`, or
// all of them.
struct PageCookieStoreGet {
  uint64_t question = 0;
  bool all = false;
  std::string name;
};

// A cookie that a CookieStore sets, as Blink's ToCookieParams() gives it to
// RestrictedCookieManager: always secure and never HttpOnly.
struct PageCookieStoreSet {
  PageCookieStoreSet();
  PageCookieStoreSet(PageCookieStoreSet&&);
  PageCookieStoreSet& operator=(PageCookieStoreSet&&);
  ~PageCookieStoreSet();

  uint64_t question = 0;
  std::string name;
  std::string value;
  // Empty for a host cookie, and otherwise with a leading dot.
  std::string domain;
  std::string path;
  // When it expires, a base::Time in microseconds since the Windows epoch: 0
  // for a session cookie, and the earliest time for one already expired.
  int64_t expiry = 0;
  // Strict, lax or no restriction.
  PageCookieSameSite same_site = PageCookieSameSite::kStrict;
  bool partitioned = false;
};

// The answer to a PageCookieStoreGet, in the order of the Session's cookie
// store.
struct PageCookieStoreCookies {
  PageCookieStoreCookies();
  PageCookieStoreCookies(PageCookieStoreCookies&&);
  PageCookieStoreCookies& operator=(PageCookieStoreCookies&&);
  ~PageCookieStoreCookies();

  uint64_t question = 0;
  std::vector<PageCookie> cookies;
};

// A change to the document's cookies that a CookieStore hears of: a cookie
// that was set, or one that went, whose value and expiry it does not hand on.
struct PageCookieStoreChange {
  bool deleted = false;
  PageCookie cookie;
};

// A message of a BroadcastChannel, as Chrome's browser process passes it
// between tabs: the channel's name, whether the message is locked to the
// agent cluster of the Page that sent it, as one that holds a WebAssembly
// module is, and the bytes of the message as SerializedScriptValue writes
// them, which the Session does not read. Its payload is the name after its
// size as a 32-bit number, the byte, and then the bytes, which are the rest.
struct PageBroadcast {
  PageBroadcast();
  PageBroadcast(const PageBroadcast&);
  PageBroadcast& operator=(const PageBroadcast&);
  PageBroadcast(PageBroadcast&&);
  PageBroadcast& operator=(PageBroadcast&&);
  ~PageBroadcast();

  // In UTF-8.
  std::string name;
  bool locked = false;
  std::string data;
};

// A frame from a Page, once checked. Which fields it uses depends on `type`.
struct FrameFromPage {
  FrameFromPage();
  FrameFromPage(FrameFromPage&&);
  FrameFromPage& operator=(FrameFromPage&&);
  ~FrameFromPage();

  PageMessage type = PageMessage::kHello;
  // kHello.
  uint32_t version = 0;
  // kRefused: why the Page refused.
  std::string reason;
  // kEvaluated.
  PageEvaluated evaluated;
  // kConsole.
  PageConsoleMessage console;
  // kUncaughtError.
  std::string uncaught_error;
  // kKnownGap.
  std::string known_gap;
  // kRequest.
  PageRequest request;
  // kCredit.
  PageCredit credit;
  // kGetCookies.
  uint64_t cookies_question = 0;
  // kSetCookie.
  std::string cookie_line;
  // kCancel.
  uint64_t cancelled = 0;
  // kCookieStoreGet.
  PageCookieStoreGet cookie_store_get;
  // kCookieStoreSet.
  PageCookieStoreSet cookie_store_set;
  // kCookieStoreListen.
  bool cookie_store_listen = false;
  // kBroadcast.
  PageBroadcast broadcast;
};

// A frame from a Session, once checked. Which fields it uses depends on
// `type`.
struct FrameFromSession {
  FrameFromSession();
  FrameFromSession(FrameFromSession&&);
  FrameFromSession& operator=(FrameFromSession&&);
  ~FrameFromSession();

  PageMessage type = PageMessage::kHello;
  // kHello.
  uint32_t version = 0;
  // kRefused: why the Session refused.
  std::string reason;
  // kStart.
  PageStart start;
  // kEvaluate.
  uint64_t call = 0;
  std::string script;
  // kResponseStarted.
  PageResponseStarted response;
  // kResponseBody.
  PageResponseBody body;
  // kResponseEnded.
  PageResponseEnded ended;
  // kCookies: the number of the question, and the line.
  uint64_t cookies_answer = 0;
  std::string cookies;
  // kCookieStoreCookies.
  PageCookieStoreCookies cookie_store_cookies;
  // kCookieStoreStored: the number of the question, and whether the cookie
  // was stored.
  uint64_t cookie_store_stored_answer = 0;
  bool cookie_store_stored = false;
  // kCookieStoreChange.
  PageCookieStoreChange cookie_store_change;
  // kBroadcastReceived.
  PageBroadcast broadcast;
};

// Returns the frame of a message of `type` with `payload`, which is at most
// kMaxPayloadSize bytes.
std::string EncodeFrame(PageMessage type, std::string_view payload);

std::string EncodeHello();
// With the start's connection.
std::string EncodeStart(const PageStart& start);

// Returns what a Page starts with, from the "start" of mitto_page_open()'s
// options, or why it is not that. It has no connection, which the library adds
// before it sends the start on.
base::expected<PageStart, std::string> ReadStart(const base::DictValue& dict);
// `script` is at most kMaxScriptSize bytes.
std::string EncodeEvaluate(uint64_t call, std::string_view script);
// `evaluated.text` is at most kMaxEvaluatedTextSize bytes.
std::string EncodeEvaluated(const PageEvaluated& evaluated);
// `message.text` is at most kMaxConsoleTextSize bytes.
std::string EncodeConsole(const PageConsoleMessage& message);
// `text` is at most kMaxPayloadSize bytes.
std::string EncodeUncaughtError(std::string_view text);
// `name` is at most kMaxPayloadSize bytes.
std::string EncodeKnownGap(std::string_view name);
// The request fits in a frame.
std::string EncodeRequest(const PageRequest& request);
std::string EncodeCancel(uint64_t number);
std::string EncodeCredit(const PageCredit& credit);
std::string EncodeGetCookies(uint64_t question);
// `line` is at most kMaxPayloadSize bytes.
std::string EncodeSetCookie(std::string_view line);
std::string EncodeLoaded();
// The URL and the headers fit in a frame.
std::string EncodeResponseStarted(const PageResponseStarted& response);
// `bytes` is at most kBodyWindow bytes.
std::string EncodeResponseBody(uint64_t number, std::string_view bytes);
std::string EncodeResponseEnded(const PageResponseEnded& ended);
// `line` is at most kMaxPayloadSize - 8 bytes.
std::string EncodeCookies(uint64_t answer, std::string_view line);
// The name fits in a frame.
std::string EncodeCookieStoreGet(const PageCookieStoreGet& get);
// The cookie fits in a frame.
std::string EncodeCookieStoreSet(const PageCookieStoreSet& set);
std::string EncodeCookieStoreListen(bool listen);
// The cookies fit in a frame.
std::string EncodeCookieStoreCookies(const PageCookieStoreCookies& cookies);
std::string EncodeCookieStoreStored(uint64_t answer, bool stored);
// The cookie fits in a frame.
std::string EncodeCookieStoreChange(const PageCookieStoreChange& change);
// Whether a broadcast fits in a frame.
bool BroadcastFits(const PageBroadcast& broadcast);
// The broadcast fits in a frame. `type` is kBroadcast or kBroadcastReceived.
std::string EncodeBroadcast(PageMessage type, const PageBroadcast& broadcast);

// Returns what a frame from a Page says, or why the Session refuses it.
base::expected<FrameFromPage, std::string> ReadFrameFromPage(
    uint32_t type,
    std::string payload);

// Returns what a frame from a Session says, or why the Page refuses it.
base::expected<FrameFromSession, std::string> ReadFrameFromSession(
    uint32_t type,
    std::string payload);

// Cuts the bytes that arrive into frames.
class FrameReader {
 public:
  enum class Result {
    kFrame,
    kNeedMore,
    // The next frame's payload is larger than kMaxPayloadSize, and nothing more
    // can be read.
    kTooLarge,
  };

  FrameReader();

  FrameReader(const FrameReader&) = delete;
  FrameReader& operator=(const FrameReader&) = delete;

  ~FrameReader();

  void Append(base::span<const uint8_t> bytes);

  // Takes the next whole frame into `type` and `payload`, if one has arrived.
  // After kTooLarge, `size` holds the size that the frame claimed.
  Result Next(uint32_t& type, std::string& payload, size_t& size);

 private:
  std::string buffer_;
  // Where the next frame starts in `buffer_`.
  size_t offset_ = 0;
};

}  // namespace mitto

#endif  // MITTO_PAGE_MESSAGES_H_
