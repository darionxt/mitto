// The native core's public C API, the contract between the native core and its
// callers, such as the Python package. Increase MITTO_ABI_VERSION on any
// incompatible change, including a change to the layout of a struct or to the
// value of a constant.
//
// Once loaded, the library stays in memory until the process exits, because its
// network thread never stops. dlclose() does not unload it, and neither does
// FreeLibrary() on Windows.

#ifndef MITTO_MITTO_H_
#define MITTO_MITTO_H_

#include <stddef.h>
#include <stdint.h>

#define MITTO_ABI_VERSION 11

// The version of the settings that mitto_session_create() reads. Adding a
// setting changes neither this nor MITTO_ABI_VERSION. Increase this one when an
// existing setting changes its meaning.
#define MITTO_SETTINGS_VERSION 1

// The version of the options that mitto_request_start() reads. Adding an option
// changes neither this nor MITTO_ABI_VERSION. Increase this one when an existing
// option changes its meaning.
#define MITTO_REQUEST_OPTIONS_VERSION 1

// The version of the cookies that mitto_session_list_cookies() writes, and that
// mitto_session_set_cookies() and mitto_session_delete_cookies() read. Adding a
// key changes neither this nor MITTO_ABI_VERSION. Increase this one when an
// existing key changes its meaning.
#define MITTO_COOKIES_VERSION 1

// The version of the options that mitto_page_open() reads. Adding an option
// changes neither this nor MITTO_ABI_VERSION. Increase this one when an existing
// option changes its meaning.
#define MITTO_PAGE_OPTIONS_VERSION 4

// A request's priority, with the values and meaning of net::RequestPriority.
#define MITTO_PRIORITY_THROTTLED 0
#define MITTO_PRIORITY_IDLE 1
#define MITTO_PRIORITY_LOWEST 2
#define MITTO_PRIORITY_LOW 3
#define MITTO_PRIORITY_MEDIUM 4
#define MITTO_PRIORITY_HIGHEST 5

// The types of event that a request goes through. Each MittoEvent field says
// which of them use it.
//
// Any number of redirects come first, then the response, and last of all the
// end of the request, which may come at any point. A request with a stream body
// also gets a MITTO_EVENT_BODY_WANTED now and then, until it has the whole body.
//
// The final response has started. Its body waits until mitto_request_read()
// asks for it.
#define MITTO_EVENT_RESPONSE_STARTED 1
// A redirect arrived and //net follows it. The next event's URL says where it
// led, unless the request ends first.
#define MITTO_EVENT_REDIRECT_FOLLOWED 2
// Body bytes that mitto_request_read() asked for have arrived.
#define MITTO_EVENT_BODY_AVAILABLE 3
// The request has ended. No event comes after this one.
#define MITTO_EVENT_FINISHED 4
// //net has taken all that mitto_request_write_body() gave it, and it is time to
// write more or to call mitto_request_end_body().
#define MITTO_EVENT_BODY_WANTED 5

// What mitto_session_next_ready() names: a request or a Page.
#define MITTO_READY_REQUEST 1
#define MITTO_READY_PAGE 2

// The types of event that a Page goes through. Each MittoPageEvent field says
// which of them use it.
//
// A script that mitto_page_evaluate() sent has its result, or has failed.
#define MITTO_PAGE_EVENT_EVALUATED 1
// The Page has ended, after each call in progress has failed with the same
// error. Each later call fails with it too.
#define MITTO_PAGE_EVENT_ENDED 2
// A script wrote a message to the console.
#define MITTO_PAGE_EVENT_CONSOLE 3
// An error that no script caught, such as a promise rejected with no handler.
// The Page goes on.
#define MITTO_PAGE_EVENT_UNCAUGHT_ERROR 4
// The navigation that mitto_page_open() started has its response, after any
// redirects, and the document it loads takes the place of about:blank's; or
// it failed before a response, and the Page keeps about:blank.
#define MITTO_PAGE_EVENT_NAVIGATED 5
// The window's load event has been dispatched: the document and its classic
// scripts have loaded and run.
#define MITTO_PAGE_EVENT_LOADED 6
// The document reached a member whose behaviour the Runtime lacks, a Known
// Gap, for the first time: each document reports each one once, as Chrome's
// use counters count a feature. The Page goes on.
#define MITTO_PAGE_EVENT_KNOWN_GAP 7

// The level of a console message, as the console method that wrote it says:
// console.debug(), console.log(), console.info(), console.warn() and
// console.error(), and the other methods as V8 sorts them.
#define MITTO_PAGE_CONSOLE_DEBUG 1
#define MITTO_PAGE_CONSOLE_LOG 2
#define MITTO_PAGE_CONSOLE_INFO 3
#define MITTO_PAGE_CONSOLE_WARNING 4
#define MITTO_PAGE_CONSOLE_ERROR 5

// Why a call to a Page failed, or why the Page ended.
//
// Nothing went wrong.
#define MITTO_PAGE_ERROR_NONE 0
// The script threw, its result is a promise that was rejected, or its result
// cannot be written as JSON, such as a BigInt or an object that holds itself.
// A MITTO_PAGE_EVENT_UNCAUGHT_ERROR holds it too. The Page goes on.
#define MITTO_PAGE_ERROR_SCRIPT 1
// The Page process ended: it crashed, ran out of memory or was killed.
#define MITTO_PAGE_ERROR_PROCESS_ENDED 2
// The library refused a message from the Page process, or the Page process one
// from the library, such as one of another version of their messages, and the
// process ended.
#define MITTO_PAGE_ERROR_REFUSED 3
// The Page's Session was destroyed.
#define MITTO_PAGE_ERROR_SESSION_DESTROYED 4

// The library defines MITTO_IMPLEMENTATION while it builds.
#if defined(_WIN32) && defined(MITTO_IMPLEMENTATION)
#define MITTO_EXPORT __declspec(dllexport)
#elif defined(_WIN32)
#define MITTO_EXPORT __declspec(dllimport)
#else
#define MITTO_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// A Session, which shares no connections or other network state with any other
// Session. Create one with mitto_session_create() and release it with
// mitto_session_destroy().
typedef struct MittoSession MittoSession;

// A request, from mitto_request_start() until mitto_request_release().
typedef struct MittoRequest MittoRequest;

// One request header. The strings need no trailing NUL byte, and the caller
// keeps them until mitto_request_start() returns.
typedef struct MittoHeader {
  const char* name;
  size_t name_size;
  const char* value;
  size_t value_size;
} MittoHeader;

// One event of a request. mitto_request_next_event() allocates the buffers and
// the caller releases them with mitto_event_free(). A buffer that this type of
// event does not use is null. Each other one ends with a NUL byte that its size
// does not count.
typedef struct MittoEvent {
  // One of the MITTO_EVENT values.
  int32_t type;
  // RESPONSE_STARTED and REDIRECT_FOLLOWED: the HTTP status code.
  int32_t status;
  // FINISHED: 0 if the whole body was read, otherwise a negative Chromium net
  // error code. An HTTP error status such as 404 is not a net error. -3
  // (ERR_ABORTED) says that the request was cancelled. A timer from the request's
  // options ends it with -118 (ERR_CONNECTION_TIMED_OUT) or -7 (ERR_TIMED_OUT).
  // A proxy that asks for credentials the Session does not have, or refuses
  // those it has, ends it with -127 (ERR_PROXY_AUTH_REQUESTED), except that a
  // SOCKS5 proxy asking a Session with none ends it with -120
  // (ERR_SOCKS_CONNECTION_FAILED).
  int32_t net_error;
  // RESPONSE_STARTED: 1 if its body comes from the Session's HTTP cache, also
  // when the server has answered a revalidation with a 304. Otherwise 0.
  int32_t from_cache;
  // RESPONSE_STARTED: the protocol it arrived over, for example "http/1.1" or
  // "h2". From the cache, the one it arrived over then.
  char* protocol;
  // RESPONSE_STARTED: the final URL, after redirects. REDIRECT_FOLLOWED: the URL
  // that answered with the redirect.
  char* url;
  // RESPONSE_STARTED and REDIRECT_FOLLOWED: the headers of that response in the
  // order received, one "name: value\r\n" line each. The status line is not
  // included.
  char* headers;
  size_t headers_size;
} MittoEvent;

// A Page, from mitto_page_open() until mitto_page_close(). Its scripts run in a
// process of its own, and its requests are its Session's.
typedef struct MittoPage MittoPage;

// One event of a Page. mitto_page_next_event() allocates the buffers and the
// caller releases them with mitto_page_event_free(). A buffer that this event
// does not use is null. Each other one ends with a NUL byte that its size does
// not count.
typedef struct MittoPageEvent {
  // One of the MITTO_PAGE_EVENT values.
  int32_t type;
  // One of the MITTO_PAGE_ERROR values: why the call failed, or why the Page
  // ended.
  int32_t error;
  // EVALUATED: the call that mitto_page_evaluate() stored.
  uint64_t call;
  // EVALUATED with no error: the result as JSON, as JSON.stringify() writes it,
  // in UTF-8. Empty if it writes nothing, as for undefined, a function or a
  // symbol. The Page process writes it, so a caller reads it as it would read
  // JSON from a server.
  char* json;
  size_t json_size;
  // Any event with an error: what went wrong, in UTF-8, such as "Uncaught
  // TypeError: x is not a function" or "the Page process was ended by signal
  // 9". CONSOLE: the message, in UTF-8, with its arguments written as V8's
  // inspector writes them for Chrome's log, separated by spaces. KNOWN_GAP:
  // the member's name, such as "URL.prototype.href" or "new Highlight". The
  // Page process writes these, so a caller reads them as it would read text
  // from a server, and they may hold a NUL character, which `message_size`
  // counts.
  char* message;
  size_t message_size;
  // CONSOLE: one of the MITTO_PAGE_CONSOLE values.
  int32_t level;
  // NAVIGATED with no net error: the response's HTTP status code.
  int32_t status;
  // NAVIGATED: 0 if the navigation has a response, and otherwise the negative
  // Chromium net error code that it failed with, as a request's
  // MITTO_EVENT_FINISHED says.
  int32_t net_error;
  // NAVIGATED: the URL of the document, after any redirects, or the URL that
  // the navigation failed at, in UTF-8 with a trailing NUL byte.
  char* url;
  size_t url_size;
} MittoPageEvent;

// Returns the ABI version of the loaded library. Callers compare it with the
// version they were written for before making any other call.
MITTO_EXPORT uint32_t mitto_abi_version(void);

// Returns the name of a Chromium net error code, as the Engine Release spells it:
// "ERR_NAME_NOT_RESOLVED" for -105, "OK" for 0 and "ERR_<unknown>" for a value
// that has no name. May be called from any thread, with no Session. The string is
// NUL-terminated and lasts as long as the process, so the caller does not free it.
MITTO_EXPORT const char* mitto_net_error_name(int32_t net_error);

// Creates a Session. May be called from any thread. The first call in a process
// also waits for the system's DNS settings, for up to a second, so that the
// first request can already see HTTPS records.
//
// `settings` is a buffer of `settings_size` bytes that holds one UTF-8 JSON
// object and needs no trailing NUL byte:
//
//   {
//     "version": 1,
//     "ignore_certificate_errors_spki_list": ["<SPKI hash>"],
//     "additional_trust_anchors": ["<text of a PEM file>"],
//     "accept_language": "en-US,en;q=0.9",
//     "user_agent": "Mozilla/5.0",
//     "origins_to_force_quic_on": ["<host>:<port>"],
//     "dns_nameserver": "<IP address>:<port>",
//     "host_resolver_rules": "MAP <host> <address>",
//     "proxy": "http://<username>:<password>@<host>:<port>",
//     "accept_ch_via_alps": {"https://<host>:<port>": "<hints>"},
//     "refused_connects": 2,
//     "http_cache": "<absolute path of a directory>",
//     "network_quality": {
//       "http_rtt_ms": 116,
//       "downstream_throughput_kbps": 2658,
//       "browser_salt": 7,
//       "renderer_salt_key": "<text>"
//     }
//   }
//
// - "version" is required and must be MITTO_SETTINGS_VERSION.
// - "ignore_certificate_errors_spki_list" accepts any certificate chain that
//   holds one of the listed keys, with the meaning of Chrome's
//   --ignore-certificate-errors-spki-list. Each entry is the Base64 form of the
//   SHA-256 hash of a DER-encoded SubjectPublicKeyInfo. The default is an empty
//   list.
// - "additional_trust_anchors" trusts the listed certificates as roots, beside
//   the Chrome Root Store. Each entry is the text of a PEM file that holds at
//   least one. //net refuses QUIC to a root that is not known, so a chain that
//   ends at one of these goes over TCP unless "origins_to_force_quic_on" lifts
//   that. The default is an empty list.
// - "accept_language" is the value of the Accept-Language header that //net adds
//   to each request whose headers do not hold one. The default is an empty
//   string, which adds no header.
// - "user_agent" is the value of the User-Agent header that //net adds to each
//   request whose headers do not hold one, and sends in what it makes by
//   itself: a CONNECT and a fetch for a certificate. The default is an empty
//   string, which adds an empty header to a request, and none to a CONNECT.
// - "origins_to_force_quic_on" reaches the listed HTTPS origins over QUIC and
//   nothing else, with the meaning of Chrome's --origin-to-force-quic-on. It
//   also lifts //net's refusal of QUIC to a certificate from an unknown root,
//   for each of their hosts. The certificate still has to verify. "*" stands
//   for every origin. The default is an empty list, so that a request learns of
//   QUIC from Alt-Svc.
// - "dns_nameserver" asks this one server in place of what the system's settings
//   name, for tests. An IPv6 address is written in brackets. The default is to
//   use the system's settings.
// - "host_resolver_rules" holds rules that change which address a name resolves
//   to, separated by commas, with the meaning of Chrome's
//   --host-resolver-rules. Only the name's resolution changes, so SNI, the
//   certificate check and the Host header keep the name. The default is an empty
//   string, which holds no rules.
// - "proxy" sends every request through this proxy, as Chrome's --proxy-server
//   does with the same one. So a request to a loopback name goes around it, and
//   a proxy that fails is not a reason to go directly. The scheme is "http",
//   "https" or "socks5", and the port defaults to 80, 443 or 1080. The URL
//   holds nothing beyond a host and port, but for a username and password,
//   percent-encoded, which answer the proxy when it asks. For "socks5", which
//   //net only offers "no authentication", the library answers by RFC 1929, and
//   each of them may be at most 255 bytes once decoded. The default is to reach
//   each server directly, whatever the system's proxy settings say.
// - "accept_ch_via_alps" says what each origin counts as having said in the
//   ACCEPT_CH frame in ALPS, for tests, in place of what //net read. The default
//   holds nothing, so what //net read counts.
// - "refused_connects" fails this many of the first connect attempts of each of
//   the Session's TCP and UDP sockets with ERR_NO_BUFFER_SPACE before they
//   reach the system, as Windows refuses one now and then, for tests. A Session
//   tries a connect that the system refused so again, with a new socket, up to
//   2 more times: on Windows always, and on another Host only when this is more
//   than 0. The default is 0.
// - "http_cache" keeps responses as Chrome's HTTP cache does, split by
//   top-level site. "memory" keeps them until the Session is destroyed. An
//   absolute path keeps them in that directory, which is made if it does not
//   exist, for any later Session too. //net only touches its subdirectory
//   Cache_Data, so the directory may hold other things. One Session at a time
//   can have it, in any process, and it stays locked until
//   mitto_session_destroy() returns or the process ends. Over HTTP/2, a cache in
//   memory cancels the stream of a 304 with an RST_STREAM, as Chrome's Incognito
//   mode does, and a directory does not, as Chrome on Windows does not. The
//   default is no cache.
// - "network_quality" works out the values of the rtt and downlink hints that a
//   request with "fetch" in its options sends, for the host of each hop, as
//   Chrome does from what its network quality estimator has, with noise for
//   each host. "http_rtt_ms" and "downstream_throughput_kbps" are that
//   estimate, each at least 0. A navigation gets the noise of the browser
//   process, whose salt "browser_salt" is from 1 to 21. Another request gets
//   the noise of the renderer process of its page's site, whose salt is 1 plus
//   the first 8 bytes of the SHA-256 of "renderer_salt_key", a space and the
//   serialized site, as a big-endian number, modulo 20. Each of the four is
//   required. A line that "fixed_headers" names keeps its value. The default
//   works out nothing, so those lines go as the headers say.
//
// An unknown key or an invalid value is refused, where Chrome's flag would
// skip an invalid entry and log it, so a mistake cannot go unnoticed.
//
// Returns 0 on success and stores the new Session in `session`. On failure,
// returns a negative Chromium net error code and stores null: -4
// (ERR_INVALID_ARGUMENT) for invalid settings, -409 (ERR_CACHE_LOCK_TIMEOUT) if
// another Session has the directory of "http_cache", without waiting for it, and
// -404 (ERR_CACHE_OPEN_FAILURE) if a cache cannot be kept there.
MITTO_EXPORT int32_t mitto_session_create(const char* settings,
                                          size_t settings_size,
                                          MittoSession** session);

// Releases `session`, closes its connections and any cache on disk, blocking
// until that is done, so that another Session can open the same cache.
// May be called from any thread. Each request still running on it ends, with a
// last event that holds -3 (ERR_ABORTED), and its handle stays valid until it is
// released. Each of its Pages ends, with a MITTO_PAGE_EVENT_ENDED event that
// holds MITTO_PAGE_ERROR_SESSION_DESTROYED, this waits for its process to end,
// and its handle stays valid until it is closed. No request may start and no other call may be made on the Session
// once this has been called. The file descriptor from mitto_session_event_fd()
// closes. A call to mitto_session_wait_for_event() that waits returns, as
// mitto_session_end_waits() makes it, and this waits for it to return before it
// releases the Session. Does nothing if `session` is null.
MITTO_EXPORT void mitto_session_destroy(MittoSession* session);

// Stores all the Session's cookies, in the order the store lists them: longest
// path first, then earliest creation. May be called from any thread, and blocks
// only the calling thread, until the network thread has answered.
//
// `cookies` receives a buffer of `cookies_size` bytes that holds one UTF-8 JSON
// object, with a trailing NUL byte that the size does not count. The caller
// releases it with mitto_cookies_free(). The format is that of
// mitto_session_set_cookies(), so what is written here can be set again. An
// expired cookie is left out, and so is one whose partition key means nothing
// beyond the frame that made it.
//
// Returns 0 on success. On failure, returns -4 (ERR_INVALID_ARGUMENT) and
// stores null and 0.
MITTO_EXPORT int32_t mitto_session_list_cookies(MittoSession* session,
                                                char** cookies,
                                                size_t* cookies_size);

// Puts cookies in the Session's store, as a response's Set-Cookie would, where
// each replaces a cookie with the same name, domain, path and partition key.
// May be called from any thread, and blocks only the calling thread, until the
// network thread has done it.
//
// `cookies` is a buffer of `cookies_size` bytes that holds one UTF-8 JSON
// object and needs no trailing NUL byte:
//
//   {
//     "version": 1,
//     "cookies": [
//       {
//         "name": "id",
//         "value": "1",
//         "domain": ".example.com",
//         "path": "/",
//         "secure": true,
//         "http_only": true,
//         "same_site": "lax",
//         "priority": "medium",
//         "expires_us": 1800000000000000,
//         "partition_key": {
//           "top_level_site": "https://example.com",
//           "has_cross_site_ancestor": false
//         },
//         "created_us": 1790000000000000,
//         "last_accessed_us": 1790000000000000,
//         "last_updated_us": 1790000000000000,
//         "source_scheme": "secure",
//         "source_port": 443,
//         "source_type": "http"
//       }
//     ]
//   }
//
// - "version" is required and must be MITTO_COOKIES_VERSION.
// - "name", "value" and "domain" are required.
// - "name", "value" and "path" are bytes, because a cookie has no fixed
//   encoding. Each character stands for the byte of that number, so one above
//   U+00FF is refused.
// - "domain" with a leading dot is a domain cookie, which also goes to
//   subdomains. Without one it is a host-only cookie.
// - "path" defaults to "/".
// - "secure" and "http_only" default to false.
// - "same_site" is "unspecified", "none", "lax" or "strict", and defaults to
//   "unspecified".
// - "priority" is "low", "medium" or "high", and defaults to "medium".
// - Each time is a whole number of microseconds since the Unix epoch, up to
//   2^53. Without "expires_us" a cookie is a session cookie.
// - "partition_key" is absent for a cookie that is not partitioned.
//
// Without "created_us" this is a new cookie, made now, which //net checks as if
// a server at its domain had sent it, and the keys below are refused.
//
// With "created_us" this is a cookie that has been in a store, so //net only
// asks that it is canonical, and brings an "expires_us" that is further away
// than it allows nearer. Any cookie that mitto_session_list_cookies() has
// written passes this. It takes these:
// - "last_accessed_us" and "last_updated_us" default to "created_us".
// - "source_scheme" is "unset", "non_secure" or "secure", and defaults to
//   "secure".
// - "source_port" is a port, -1 for none or -2 for an invalid one, and defaults
//   to 443.
// - "source_type" is "http", "script" or "other", and defaults to "other".
//
// An unknown key or an invalid value is refused, and then no cookie is set, so
// a mistake cannot go unnoticed.
//
// Returns 0 on success. On failure, returns -4 (ERR_INVALID_ARGUMENT).
MITTO_EXPORT int32_t mitto_session_set_cookies(MittoSession* session,
                                               const char* cookies,
                                               size_t cookies_size);

// Deletes the Session's cookies that match a filter, and stores how many went.
// May be called from any thread, and blocks only the calling thread, until the
// network thread has done it.
//
// `filter` is a buffer of `filter_size` bytes that holds one UTF-8 JSON object
// and needs no trailing NUL byte:
//
//   {"version": 1, "name": "id", "domain": ".example.com", "path": "/"}
//
// - "version" is required and must be MITTO_COOKIES_VERSION.
// - Each of the others must equal the cookie's, written as in
//   mitto_session_set_cookies(), so "example.com" does not match a cookie of
//   ".example.com". Without any of them, every cookie matches.
//
// An unknown key or an invalid value is refused.
//
// Returns 0 on success. On failure, returns -4 (ERR_INVALID_ARGUMENT) and
// stores 0.
MITTO_EXPORT int32_t mitto_session_delete_cookies(MittoSession* session,
                                                  const char* filter,
                                                  size_t filter_size,
                                                  uint32_t* deleted);

// Releases a buffer from mitto_session_list_cookies(). Does nothing if
// `cookies` is null.
MITTO_EXPORT void mitto_cookies_free(char* cookies);

// Stores a file descriptor that is readable while any of the Session's requests
// or Pages has an event that has not been named by mitto_session_next_ready().
// May be called from any thread, and each call stores the same one, which the
// Session owns: wait on it, but do not read from it or close it.
//
// This is for a caller that cannot block, such as an event loop. A caller gets
// this before it starts requests, because only events that arrive after the
// first call to this or to mitto_session_wait_for_event() count.
//
// Returns 0 on success. On failure, such as when the process has too many open
// files, returns a negative Chromium net error code and stores -1. On Windows a
// Session has no such descriptor, so it fails with -11 (ERR_NOT_IMPLEMENTED)
// where it would succeed elsewhere. An event loop there has a thread wait in
// mitto_session_wait_for_event() instead.
MITTO_EXPORT int32_t mitto_session_event_fd(MittoSession* session, int32_t* fd);

// Stores the handle behind the next waiting event, a request's or a Page's,
// and takes that entry off the list that keeps the file descriptor readable and
// that mitto_session_wait_for_event() waits for. `kind` receives
// MITTO_READY_REQUEST for a MittoRequest*, whose event
// mitto_request_next_event() takes, or MITTO_READY_PAGE for a MittoPage*, whose
// event mitto_page_next_event() takes. A handle with several events waiting is
// named once for each. May be called from any thread.
//
// The caller uses this only to find the request or Page among the handles it
// holds, and then takes the event. The handle may have been released or
// closed, or even reused by a newer one, which then has no event yet. So do not
// use a stored pointer that is not among the handles held, and expect the next
// event to be missing at times.
//
// Returns 0 on success, and -1 (ERR_IO_PENDING) with 0 and null stored if no
// event is waiting.
MITTO_EXPORT int32_t mitto_session_next_ready(MittoSession* session,
                                              int32_t* kind,
                                              void** handle);

// Blocks until mitto_session_next_ready() would name a request or Page, for a
// caller that waits on a thread of its own for an event loop that cannot watch
// the file descriptor, as on Windows. May be called from any thread.
//
// It returns once each time the list goes from empty to naming a handle: after
// a return, it waits until mitto_session_next_ready() has emptied the list and
// another event has arrived. So a thread that hands each return to an
// event loop sleeps while the loop has not caught up, however many events wait.
// Only events that arrive after the first call to this or to
// mitto_session_event_fd() count, so a caller makes one before it starts
// requests, with a `timeout_ms` of 0 so as not to block.
//
// Blocks for up to `timeout_ms` milliseconds if that is positive, not at all if
// it is 0, and for as long as it takes if it is negative.
//
// Returns 0 when a handle waits to be named, -1 (ERR_IO_PENDING) if none came
// in time, -3 (ERR_ABORTED) at once after mitto_session_end_waits(), and -4
// (ERR_INVALID_ARGUMENT) for a null `session`.
MITTO_EXPORT int32_t mitto_session_wait_for_event(MittoSession* session,
                                                  int64_t timeout_ms);

// Ends each call to mitto_session_wait_for_event() that waits, and makes each
// later one return -3 (ERR_ABORTED) at once. A caller ends its waiting thread
// this way before it destroys the Session, because no call may start once
// mitto_session_destroy() has been called. May be called from any thread, and
// does not wait for anything. Does nothing if `session` is null.
MITTO_EXPORT void mitto_session_end_waits(MittoSession* session);

// Starts a request for `url`, a NUL-terminated UTF-8 string, on the library's
// network thread, and returns without waiting for it. May be called from any
// thread, also while other requests run on the same Session. //net decides what
// a redirect does to the method and the body, as it does in Chrome, and ends the
// request with ERR_TOO_MANY_REDIRECTS after 20 of them. An Authorization or
// Cookie header in `headers` does not go to another origin, and stays gone if a
// later redirect comes back. The cookies that //net adds from the store go as
// //net decides on each hop, from what the "fetch" option says.
//
// `headers` points to `header_count` headers, in the order they are to reach
// the wire, and may be null if the count is 0. A name may appear only once,
// ignoring case. //net adds its own headers as it does in Chrome, such as Host
// before these and Accept-Encoding after them, unless they are among these.
// Referer cannot be among these, because //net only sends the one that
// "referrer" or "fetch" gives it.
//
// `options` is a buffer of `options_size` bytes that holds one UTF-8 JSON object
// and needs no trailing NUL byte:
//
//   {
//     "version": 1,
//     "method": "GET",
//     "priority": 2,
//     "priority_incremental": true,
//     "referrer": "https://example.com/",
//     "follow_redirects": false,
//     "stream_body": true,
//     "stream_body_size": 1073741824,
//     "connect_timeout_ms": 5000,
//     "read_timeout_ms": 5000,
//     "total_timeout_ms": 30000,
//     "fetch": {
//       "mode": "cors",
//       "destination": "empty",
//       "credentials_mode": "same-origin",
//       "has_user_activation": false,
//       "initiator": "https://example.com/page",
//       "fixed_headers": ["accept"]
//     }
//   }
//
// - "version" is required and must be MITTO_REQUEST_OPTIONS_VERSION.
// - "method" is the HTTP method, sent as it is written here. Chrome never sends
//   CONNECT, TRACE or TRACK, so they are refused. The default is "GET".
// - "priority" is one of the MITTO_PRIORITY values, and "priority_incremental"
//   has the meaning of net::URLRequest::SetPriorityIncremental(). Together they
//   decide the HTTP/2 weight, the priority header and the HTTP/3
//   PRIORITY_UPDATE. The defaults are those of //net: MITTO_PRIORITY_LOWEST and
//   true.
// - "referrer" is the URL to send as the Referer header, on each hop. //net puts
//   it after `headers` and before the headers it adds, and leaves out any
//   username, password and fragment. A URL that //net cannot send as a referrer,
//   such as one that is not valid, is refused. The default leaves it to "fetch",
//   or sends none if there is no "fetch".
// - "follow_redirects" set to false makes a redirect the response, whose body is
//   empty because //net does not read the body of a redirect. A redirect to a URL
//   that is not valid still ends the request with ERR_INVALID_REDIRECT, because
//   //net checks that first. The default is true.
// - "stream_body" set to true says that the body comes through
//   mitto_request_write_body(), and `body` has to be null. Without a
//   "stream_body_size" it is chunked, with no Content-Length, and //net refuses to
//   send it over HTTP/1.1, as it does for a stream in Chrome: the request finishes
//   with ERR_ALPN_NEGOTIATION_FAILED and nothing of it reaches the server. A stream
//   can be read only once, so any redirect but a 303 finishes the request with
//   ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED, as Fetch fails it, and so does another
//   attempt on a new connection once more than 64 KiB has gone. A 303 drops the
//   stream, and later redirects of that request go as they do with no body. The
//   default is false.
// - "stream_body_size" is how many bytes the stream holds, a whole number up to
//   2^53, which needs "stream_body" as well. //net sends it as the Content-Length,
//   over any protocol. A write past it is refused, and ending the body before it
//   finishes the request with ERR_UPLOAD_FILE_CHANGED.
// - "connect_timeout_ms" limits the time from the start of the request, or from a
//   redirect, until net::URLRequest::Delegate::OnConnected() runs: the name is
//   resolved, a connection is free and any proxy tunnel and TLS handshake are
//   done. Past it, the request finishes with ERR_CONNECTION_TIMED_OUT.
// - "read_timeout_ms" limits each wait for bytes from the server. One wait runs
//   from OnConnected() until the headers have arrived, which //net does not report
//   a byte at a time. Others run while mitto_request_read() has asked for body
//   bytes that have not come, so a caller who does not read never hits it. Bytes
//   that a content decoder holds back still count as arriving, which can stretch
//   a wait to almost twice the limit. Bytes of the body on their way to the server
//   count too, so a large body can take longer than the limit, as long as it keeps
//   moving. A wait for the next bytes of a stream body is a wait for the caller and
//   does not count. Past it, the request finishes with ERR_TIMED_OUT.
// - "total_timeout_ms" limits the time from the start of the request to its end,
//   over all its redirects and however fast the caller reads. Past it, the request
//   finishes with ERR_TIMED_OUT.
// - Each timer takes a positive number of milliseconds up to 2147483647. The
//   default is no timer, which leaves only the limits that //net has of its own.
// - "fetch" says what a request made for a page says of itself, in the terms of
//   Chrome's network::ResourceRequest, so that this does to it what Chrome does,
//   at its start and on each hop. A line in `headers` keeps its place and gets its
//   value from these rules, or goes, and one they add goes last. See below for
//   what is in it. Without it, a request sends what `headers` and "referrer" say
//   and no more, and counts as a navigation that the browser started.
//
// In "fetch":
// - "mode" is "navigate", "cors" or "no-cors", and "destination" is what
//   Sec-Fetch-Dest says. "credentials_mode" is "same-origin" or "include". All
//   three are required.
// - "has_user_activation" says whether Sec-Fetch-User goes. The default is false.
// - "initiator" is the URL of the page the request comes from, which counts as a
//   top-level page. It decides Sec-Fetch-Site, Referer and Origin, which SameSite
//   cookies go, and the partition of connections and of partitioned cookies. A URL
//   that is not valid is refused. Without it, a "navigate" counts as started by
//   the browser, and another as coming from the page at the root of the origin of
//   its URL.
// - "fixed_headers" holds names in any case. A line of one of these names stays as
//   `headers` says, or stays absent, on each hop, whatever the rules would make of
//   it. //net still changes an Origin on a redirect as it does in Chrome. "referer"
//   here with no "referrer" sends none.
//
// An unknown key or an invalid value is refused, so a mistake cannot go
// unnoticed.
//
// `body` points to `body_size` bytes to send as the request's body, which this
// copies before it returns. //net adds a Content-Length, and sends the bytes as
// it does in Chrome. Null sends no body, and needs a `body_size` of 0, and then
// //net adds a Content-Length of 0 only for POST and PUT. A body that is not null
// and has a `body_size` of 0 is an empty body, which adds a Content-Length of 0
// for any method.
//
// Returns 0 on success and stores the handle of the new request, which the
// caller later passes to mitto_request_release(). What goes wrong after that,
// such as ERR_NAME_NOT_RESOLVED or an invalid URL, arrives as the
// MITTO_EVENT_FINISHED event. On failure, returns -4 (ERR_INVALID_ARGUMENT) and
// stores null.
MITTO_EXPORT int32_t mitto_request_start(MittoSession* session,
                                         const char* url,
                                         const MittoHeader* headers,
                                         size_t header_count,
                                         const char* options,
                                         size_t options_size,
                                         const uint8_t* body,
                                         size_t body_size,
                                         MittoRequest** request);

// Takes the request's next event into `event`, which the caller then passes to
// mitto_event_free(). May be called from any thread.
//
// Events wait in a queue inside the library, for as long as the caller takes.
// This call blocks only the calling thread: for up to `timeout_ms` milliseconds
// if that is positive, not at all if it is 0, and for as long as it takes if it
// is negative.
//
// Returns 0 on success. Returns -1 (ERR_IO_PENDING) if no event arrived in time,
// and -4 (ERR_INVALID_ARGUMENT) for an invalid argument or once the
// MITTO_EVENT_FINISHED event has been taken, in which case there will never be
// another. In these cases it leaves `event` zeroed, so it needs no freeing.
MITTO_EXPORT int32_t mitto_request_next_event(MittoRequest* request,
                                              int64_t timeout_ms,
                                              MittoEvent* event);

// Copies up to `size` bytes of the response's body, after any content decoding,
// into `buffer`, and returns how many. May be called from any thread, and never
// blocks.
//
// The library reads the body from the network only when this asks for it, and
// holds a small part of it at most, so a caller who does not read makes the
// server wait through flow control. If no bytes are waiting, this asks for more
// and returns -1 (ERR_IO_PENDING). A MITTO_EVENT_BODY_AVAILABLE event says when
// to call it again, or a MITTO_EVENT_FINISHED event says that no more will come.
// It may be called before the response has started, and the library then starts
// to read as soon as it has.
//
// Returns 0 once the request has finished and the bytes that arrived before that
// have been read, and -4 (ERR_INVALID_ARGUMENT) for an invalid argument, such as
// a `size` of 0.
MITTO_EXPORT int32_t mitto_request_read(MittoRequest* request,
                                        uint8_t* buffer,
                                        size_t size);

// Hands over the next bytes of a stream body, which the "stream_body" option of
// mitto_request_start() announces. Copies up to `size` bytes from `data` and
// returns how many. May be called from any thread, and never blocks.
//
// The library holds a small part of the body at most, and takes no more until
// //net has taken all of that, so a server that does not read makes the caller
// wait through flow control. The first MITTO_EVENT_BODY_WANTED event comes when
// //net can use the first bytes, so a request that fails before that has read none
// of the body, and a "stream_body_size" of 0 gets one at that same point. Each
// later one says that all that was written has gone. Writing before an event is
// allowed, and returns -1 (ERR_IO_PENDING) if there is no room yet, which the next
// event will say has changed.
//
// Returns 0 if nothing more of the body is wanted, because the request has
// finished or a 303 has dropped the body. Returns -4 (ERR_INVALID_ARGUMENT) for an
// invalid argument, such as a `size` of 0, for a request with no stream, after
// mitto_request_end_body() and for bytes past "stream_body_size".
MITTO_EXPORT int32_t mitto_request_write_body(MittoRequest* request,
                                              const uint8_t* data,
                                              size_t size);

// Says that nothing more of the stream body will come. May be called from any
// thread, and never blocks. A body that has failed on the caller's side ends with
// mitto_request_cancel() instead, so that the server does not take it for the
// whole body.
//
// Returns 0 on success, also if the request has finished. Returns -4
// (ERR_INVALID_ARGUMENT) for a request with no stream, and after the first call.
MITTO_EXPORT int32_t mitto_request_end_body(MittoRequest* request);

// Ends the request and returns without waiting for that. May be called from any
// thread, and does nothing if the request has finished or `request` is null. Its
// connection ends if its body has not been read, except over a protocol that
// can end one stream, where other requests carry on using it.
//
// Unless the request finishes first, its last event holds -3 (ERR_ABORTED).
MITTO_EXPORT void mitto_request_cancel(MittoRequest* request);

// Cancels the request if it is still running and releases its handle, along with
// any events that have not been taken. May be called from any thread, but not
// while another call is using the handle. Does nothing if `request` is null.
MITTO_EXPORT void mitto_request_release(MittoRequest* request);

// Releases the buffers of an event filled by mitto_request_next_event() and
// zeroes it. Does nothing if `event` is null or already zeroed.
MITTO_EXPORT void mitto_event_free(MittoEvent* event);

// Opens a Page on `session`: starts the Page program, from the directory that
// the library was loaded from, in a process of its own, and returns without
// waiting for it to start. May be called from any thread.
//
// `options` is a buffer of `options_size` bytes that holds one UTF-8 JSON object
// and needs no trailing NUL byte:
//
//   {
//     "version": 4,
//     "start": {"time_zone": "America/New_York", "locale": "en-US",
//               "processors": 8, "touch_points": 0,
//               "work_area_x": 0, "work_area_y": 0,
//               "work_area_width": 1920, "work_area_height": 1032,
//               "window_state": "maximised",
//               "window_x": 0, "window_y": 0,
//               "window_width": 1920, "window_height": 1032,
//               "contrast_theme": "none", "inverted_colors": "none",
//               "memory": 8, "screen_width": 1920, "screen_height": 1080,
//               "scale": 1, "viewport_width": 1920, "viewport_height": 945,
//               "color_scheme": "light", "reduced_motion": "no-preference",
//               "reduced_transparency": "no-preference",
//               "effective_type": "4g",
//               "navigator": {
//                 "user_agent": "Mozilla/5.0 ...", "platform": "Win32",
//                 "languages": "en-US,en",
//                 "user_agent_data": {
//                   "brands": [{"brand": "Chromium", "version": "157"}, ...],
//                   "full_version_list": [...], "mobile": false,
//                   "platform": "Windows", "platform_version": "19.0.0",
//                   "architecture": "x86", "bitness": "64", "model": "",
//                   "ua_full_version": "157.0.8103.39", "wow64": false,
//                   "form_factors": ["Desktop"]}},
//               "features": {"enable_blink_features": [],
//                            "disable_blink_features": ["CanvasDrawElement"],
//                            "v8_flags": ["--sqrt_allocation_limits"]}},
//     "url": "https://example.com/",
//     "requests": {
//       "navigation": {"headers": [["sec-ch-ua", "..."], ...],
//                      "options": {"version": 1, "fetch": {...}, ...}},
//       "script": {...},
//       "fetch": {...},
//       "fetch_with_body": {...}
//     }
//   }
//
// - "version" is required and must be MITTO_PAGE_OPTIONS_VERSION.
// - "start" is required: what the Page starts with, which it would otherwise
//   read from the machine and the browser, as the Session's Machine, Persona
//   and Accept-Language say, with every key above and no other. Places and
//   sizes are in device-independent pixels, from the display's top left corner:
//   - "time_zone" is the IANA name of the time zone that the Page's Date and
//     Intl report, and "locale" the BCP 47 tag of the locale that its Intl
//     falls back to. The Page process checks both, because only it holds ICU's
//     whole data. If it does not know one, it refuses it, and the Page ends
//     with MITTO_PAGE_ERROR_REFUSED. Each is at most 64 characters of ASCII
//     letters, digits, '_', '-', '+' and '/'.
//   - "processors" is the number of logical processors, from 1 to 64, and
//     "touch_points" how many touch points the touch screen takes, from 0, for
//     none, to 256.
//   - "work_area_x", "work_area_y", "work_area_width" and "work_area_height" are
//     the display less the taskbar. Its place is at least 0 and its size at
//     least 1.
//   - "window_state" is "maximised" or "normal", and "window_x", "window_y",
//     "window_width" and "window_height" are Chrome's window, with its bars and
//     borders, whose size is at least 1.
//   - "contrast_theme" is Windows' contrast theme: "none", "aquatic", "desert",
//     "dusk", "night-sky", "high-contrast-1", "high-contrast-2",
//     "high-contrast-black" or "high-contrast-white". "inverted_colors" is
//     "inverted" with Windows' colour filter that inverts colours, or "none".
//   - "memory" is what Chrome makes of the memory, 2, 4, 8, 16 or 32 GiB, from
//     which the Page's V8 also works out its heap's limits.
//   - "screen_width" and "screen_height" are the primary display, "scale" its
//     scale, from 1 to 5, and "viewport_width" and "viewport_height" the
//     window's page area. Each size is at least 1.
//   - "color_scheme" is "light" or "dark", "reduced_motion" and
//     "reduced_transparency" each "no-preference" or "reduce", and
//     "effective_type" the effective connection type that the Session's ect
//     hint gives: "slow-2g", "2g", "3g" or "4g".
//   - "navigator" is what the browser tells the Page of itself: "user_agent",
//     the Session's User-Agent, "platform", what navigator.platform gives,
//     "languages", the languages of Chrome's setting separated by commas, from
//     which the Session's Accept-Language comes, and "user_agent_data", the
//     values of the Session's user agent client hints: "brands" and
//     "full_version_list", each a list of {"brand", "version"}, "mobile" and
//     "wow64", each a boolean, "form_factors", a list of strings, and
//     "platform", "platform_version", "architecture", "bitness", "model" and
//     "ua_full_version", each a string. The user agent and the languages are
//     each at most 4096 bytes of UTF-8, the other strings at most 256, and the
//     lists at most 32 items long.
//   - "features" is what the Page turns on and off beside the Engine Release's
//     runtime-enabled features and V8's defaults, as the Session's Persona
//     says, with these three keys and no other. "enable_blink_features" and
//     "disable_blink_features" name runtime-enabled features to turn on, and
//     then off, as Chrome's --enable-blink-features and
//     --disable-blink-features do, at most 4096 each, each at most 128 ASCII
//     letters, digits and '_'. The Page process refuses a name that is not a
//     feature, because only it holds the bindings. "v8_flags" are V8's flags,
//     at most 256, set in their order after the Engine Release's, each as gin
//     writes one: "--name", "--no-name" or "--name=value", at most 256
//     characters, with no space.
//   The library works out what the Page's navigator.connection gives from the
//   Session's network estimate.
// - "url" is where the Page navigates once it opens, through the Session, as a
//   request of the navigation template does without an Initiator. The
//   response, after any redirects, is the Page's document, and its URL the
//   document's. A MITTO_PAGE_EVENT_NAVIGATED event says how it went, and
//   MITTO_PAGE_EVENT_LOADED when the document has loaded. Without "url" the
//   Page stays at about:blank.
// - "requests" holds what a request of each Request Kind sends, as the
//   Session's Persona says: "headers", the header lines as [name, value] pairs
//   in the order mitto_request_start() takes them, each character at most
//   U+00FF, and "options", what mitto_request_start() reads, with "fetch" and
//   without "initiator", "fixed_headers", "referrer", a stream body or
//   timers. "navigation" is required with "url". The Page's own requests use
//   "script" and "fetch", with the Page's document as their Initiator, and fail
//   with ERR_NOT_IMPLEMENTED for a kind that is not here. A fetch() with a body
//   uses "fetch_with_body", whose Content-Type line is where its Persona puts
//   it: the script's own Content-Type takes its place, or it goes if the script
//   gives none. A script's other lines take the places of the template's lines
//   of their names, and the rest follow, as a caller's headers do. A
//   cross-origin fetch() reaches the Page only if its response passes the CORS
//   check for a request without credentials, and one that would need a CORS
//   preflight fails with ERR_NOT_IMPLEMENTED.
//
// The Page's requests and cookies are the Session's: its scripts' requests go
// through the Session's connections and cookies, as a request of their kind
// does, and document.cookie reads and writes the Session's cookies for the
// document's URL as a top-level page's script does, so it never sees an
// HttpOnly cookie and a cookie it sets is one set by script.
//
// An unknown key or an invalid value is refused, so a mistake cannot go
// unnoticed.
//
// The Page process holds its end of the channel to the library, and the
// standard output and error of this process, and opens no file or socket of its
// own. It ends when mitto_page_close() closes the Page, when its Session is
// destroyed, and when this process ends without either. If it ends before
// that, because it crashed, ran out of memory or was killed, its calls in
// progress and each later one fail with MITTO_PAGE_ERROR_PROCESS_ENDED, and the
// Session's other Pages and requests go on.
//
// Returns 0 on success and stores the Page, which the caller later passes to
// mitto_page_close(). On failure, returns a negative Chromium net error code and
// stores null: -4 (ERR_INVALID_ARGUMENT) for invalid options, -6
// (ERR_FILE_NOT_FOUND) if there is no Page program beside the library, as in a
// wheel that a release made, -12 (ERR_INSUFFICIENT_RESOURCES) if the process has
// no room for the channel, and -2 (ERR_FAILED) if the program cannot start.
MITTO_EXPORT int32_t mitto_page_open(MittoSession* session,
                                     const char* options,
                                     size_t options_size,
                                     MittoPage** page);

// Stores the ID of the Page's process, which stays the same after the process
// has ended. May be called from any thread.
//
// Returns 0 on success. On failure, returns -4 (ERR_INVALID_ARGUMENT) and
// stores 0.
MITTO_EXPORT int32_t mitto_page_process_id(MittoPage* page,
                                           int64_t* process_id);

// Sends `script`, `script_size` bytes of UTF-8 that need no trailing NUL byte,
// to the Page, which runs it as a classic script in its context, and returns
// without waiting for it. May be called from any thread. Scripts run in the
// order they are sent, in the same context, so each sees what the ones before
// it left there.
//
// The result comes as a MITTO_PAGE_EVENT_EVALUATED event with the number that
// this stores in `call`. Each call gets one such event, also when it fails.
// When the result is a promise, the event comes once the promise settles,
// with what it was fulfilled with or why it was rejected, and the calls after
// it go on meanwhile. So results come in any order. A promise that nothing
// settles keeps its call in progress until the Page ends.
//
// Returns 0 on success. On failure, returns -4 (ERR_INVALID_ARGUMENT) for an
// invalid argument, such as a script that is not UTF-8, or -142
// (ERR_MSG_TOO_BIG) for a script of more than 16 MiB less 8 bytes, and stores 0.
MITTO_EXPORT int32_t mitto_page_evaluate(MittoPage* page,
                                         const char* script,
                                         size_t script_size,
                                         uint64_t* call);

// Takes the Page's next event into `event`, which the caller then passes to
// mitto_page_event_free(). May be called from any thread.
//
// Events wait in a queue inside the library, for as long as the caller takes.
// This call blocks only the calling thread: for up to `timeout_ms` milliseconds
// if that is positive, not at all if it is 0, and for as long as it takes if it
// is negative.
//
// Returns 0 on success, -1 (ERR_IO_PENDING) if no event arrived in time, and -4
// (ERR_INVALID_ARGUMENT) for an invalid argument. In these cases it leaves
// `event` zeroed, so it needs no freeing.
MITTO_EXPORT int32_t mitto_page_next_event(MittoPage* page,
                                           int64_t timeout_ms,
                                           MittoPageEvent* event);

// Releases the buffers of an event filled by mitto_page_next_event() and zeroes
// it. Does nothing if `event` is null or already zeroed.
MITTO_EXPORT void mitto_page_event_free(MittoPageEvent* event);

// Ends each call to mitto_page_next_event() on `page` that waits, which returns
// -1 (ERR_IO_PENDING), and makes each later one that finds no event return so
// at once. A caller ends a wait on another thread this way before it closes
// the Page, because no other call may use the handle then. May be called from
// any thread, also while another thread waits in mitto_page_next_event(), and
// does not wait for anything. Does nothing if `page` is null.
MITTO_EXPORT void mitto_page_end_waits(MittoPage* page);

// Closes the Page and releases its handle, along with any events that have not
// been taken. It closes the channel, waits for the Page process to end, for up
// to 2 seconds before it kills it, and returns once the process has gone. May
// be called from any thread, also after the Session has been destroyed, but not
// while another call is using the handle. Does nothing if `page` is null.
MITTO_EXPORT void mitto_page_close(MittoPage* page);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MITTO_MITTO_H_
