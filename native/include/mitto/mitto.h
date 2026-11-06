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

#define MITTO_ABI_VERSION 4

// The version of the settings that mitto_session_create() reads. Adding a
// setting changes neither this nor MITTO_ABI_VERSION. Increase this one when an
// existing setting changes its meaning.
#define MITTO_SETTINGS_VERSION 1

// The version of the options that mitto_request_start() reads. Adding an option
// changes neither this nor MITTO_ABI_VERSION. Increase this one when an existing
// option changes its meaning.
#define MITTO_REQUEST_OPTIONS_VERSION 1

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
// end of the request, which may come at any point.
//
// The final response has started. Its body waits until mitto_request_read()
// asks for it.
#define MITTO_EVENT_RESPONSE_STARTED 1
// A redirect arrived and //net follows it.
#define MITTO_EVENT_REDIRECT_FOLLOWED 2
// Body bytes that mitto_request_read() asked for have arrived.
#define MITTO_EVENT_BODY_AVAILABLE 3
// The request has ended. No event comes after this one.
#define MITTO_EVENT_FINISHED 4

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
  // (ERR_ABORTED) says that the request was cancelled.
  int32_t net_error;
  // RESPONSE_STARTED: the protocol it arrived over, for example "http/1.1" or
  // "h2".
  char* protocol;
  // RESPONSE_STARTED: the final URL, after redirects. REDIRECT_FOLLOWED: the URL
  // that it leads to.
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
//     "accept_language": "en-US,en;q=0.9",
//     "origins_to_force_quic_on": ["<host>:<port>"],
//     "dns_nameserver": "<IP address>:<port>",
//     "host_resolver_rules": "MAP <host> <address>"
//   }
//
// - "version" is required and must be MITTO_SETTINGS_VERSION.
// - "ignore_certificate_errors_spki_list" accepts any certificate chain that
//   holds one of the listed keys, with the meaning of Chrome's
//   --ignore-certificate-errors-spki-list. Each entry is the Base64 form of the
//   SHA-256 hash of a DER-encoded SubjectPublicKeyInfo. The default is an empty
//   list.
// - "accept_language" is the value of the Accept-Language header that //net adds
//   to each request whose headers do not hold one. The default is an empty
//   string, which adds no header.
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
// released. No request may start once this has been called. The file descriptor
// from mitto_session_event_fd() closes. Does nothing if `session` is null.
MITTO_EXPORT void mitto_session_destroy(MittoSession* session);

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
// thread, also while other requests run on the same Session. Redirects are
// followed as //net decides.
//
// `headers` points to `header_count` headers, in the order they are to reach
// the wire, and may be null if the count is 0. A name may appear only once,
// ignoring case. //net adds its own headers as it does in Chrome, such as Host
// before these and Accept-Encoding after them, unless they are among these.
// Referer cannot be among these, because //net only sends the "referrer" option.
//
// `options` is a buffer of `options_size` bytes that holds one UTF-8 JSON object
// and needs no trailing NUL byte:
//
//   {
//     "version": 1,
//     "method": "GET",
//     "priority": 2,
//     "priority_incremental": true,
//     "referrer": "https://example.com/"
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
// - "referrer" is the URL to send as the Referer header. //net puts it after
//   `headers` and before the headers it adds, and leaves out any username,
//   password and fragment. A URL that //net cannot send as a referrer, such as
//   one that is not valid, is refused. The default is to send none.
//
// An unknown key or an invalid value is refused, so a mistake cannot go
// unnoticed.
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
