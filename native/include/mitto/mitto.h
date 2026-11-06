// The native core's public C API, the contract between the native core and its
// callers, such as the Python package. Increase MITTO_ABI_VERSION on any
// incompatible change, including a change to the layout of a struct or to the
// value of a constant.
//
// Once loaded, the library stays in memory until the process exits, because its
// network thread never stops. dlclose() does not unload it.

#ifndef MITTO_MITTO_H_
#define MITTO_MITTO_H_

#include <stddef.h>
#include <stdint.h>

#define MITTO_ABI_VERSION 6

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

#define MITTO_EXPORT __attribute__((visibility("default")))

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
  // RESPONSE_STARTED: the protocol it arrived over, for example "http/1.1" or
  // "h2".
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
//     "proxy": "http://<username>:<password>@<host>:<port>"
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
//
// An unknown key or an invalid value is refused, where Chrome's flag would
// skip an invalid entry and log it, so a mistake cannot go unnoticed.
//
// Returns 0 on success and stores the new Session in `session`. On failure,
// returns a negative Chromium net error code, -4 (ERR_INVALID_ARGUMENT) for
// invalid settings, and stores null.
MITTO_EXPORT int32_t mitto_session_create(const char* settings,
                                          size_t settings_size,
                                          MittoSession** session);

// Releases `session` and closes its connections, blocking until that is done.
// May be called from any thread. Each request still running on it ends, with a
// last event that holds -3 (ERR_ABORTED), and its handle stays valid until it is
// released. No request may start and no call for cookies may be made once this
// has been called. The file descriptor from mitto_session_event_fd() closes.
// Does nothing if `session` is null.
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
// has an event that has not been named by mitto_session_next_ready_request().
// May be called from any thread, and each call stores the same one, which the
// Session owns: wait on it, but do not read from it or close it.
//
// This is for a caller that cannot block, such as an event loop. A caller gets
// this before it starts requests, because only events that arrive after the
// first call count.
//
// Returns 0 on success. On failure, such as when the process has too many open
// files, returns a negative Chromium net error code and stores -1.
MITTO_EXPORT int32_t mitto_session_event_fd(MittoSession* session, int32_t* fd);

// Stores the request behind the next waiting event, and takes that entry off the
// list that keeps the file descriptor readable. A request with several events
// waiting is named once for each. May be called from any thread.
//
// The caller uses this only to find the request among the handles it holds, and
// then gets the event with mitto_request_next_event(). The handle may have been
// released, or even reused by a newer request, which then has no event yet. So
// do not use a stored pointer that is not among the handles held, and expect
// mitto_request_next_event() to find nothing at times.
//
// Returns 0 on success, and -1 (ERR_IO_PENDING) with null stored if no event is
// waiting.
MITTO_EXPORT int32_t mitto_session_next_ready_request(MittoSession* session,
                                                      MittoRequest** request);

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

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MITTO_MITTO_H_
