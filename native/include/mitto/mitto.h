// The native core's public C API, the contract between the native core and its
// callers, such as the Python package. Increase MITTO_ABI_VERSION on any
// incompatible change, including a change to the layout of a struct or to the
// value of a constant.
//
// Once loaded, the library stays in memory until the process exits, because its
// network thread never stops. dlclose() does not unload it.

#ifndef MITTO_MITTO_H_
#define MITTO_MITTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MITTO_ABI_VERSION 3

// The version of the settings that mitto_session_create() reads. Adding a
// setting changes neither this nor MITTO_ABI_VERSION. Increase this one when an
// existing setting changes its meaning.
#define MITTO_SETTINGS_VERSION 1

// A request's priority, with the values and meaning of net::RequestPriority.
#define MITTO_PRIORITY_THROTTLED 0
#define MITTO_PRIORITY_IDLE 1
#define MITTO_PRIORITY_LOWEST 2
#define MITTO_PRIORITY_LOW 3
#define MITTO_PRIORITY_MEDIUM 4
#define MITTO_PRIORITY_HIGHEST 5

#define MITTO_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

// A Session, which shares no connections or other network state with any other
// Session. Create one with mitto_session_create() and release it with
// mitto_session_destroy().
typedef struct MittoSession MittoSession;

// One request header. The strings need no trailing NUL byte, and the caller
// keeps them until the request returns.
typedef struct MittoHeader {
  const char* name;
  size_t name_size;
  const char* value;
  size_t value_size;
} MittoHeader;

// A finished response. mitto_session_request() allocates the buffers and the
// caller releases them with mitto_response_free(). Each buffer ends with a NUL
// byte that its size does not count.
typedef struct MittoResponse {
  // The HTTP status code of the final response, after redirects.
  int32_t status;
  // The protocol the response arrived over, for example "http/1.1" or "h2".
  char* protocol;
  // The final response's headers in the order received, one "name: value\r\n"
  // line each. The status line is not included.
  char* headers;
  size_t headers_size;
  // The final response's body, after any content decoding.
  uint8_t* body;
  size_t body_size;
} MittoResponse;

// Returns the ABI version of the loaded library. Callers compare it with the
// version they were written for before making any other call.
MITTO_EXPORT uint32_t mitto_abi_version(void);

// Creates a Session. May be called from any thread.
//
// `settings` is a buffer of `settings_size` bytes that holds one UTF-8 JSON
// object and needs no trailing NUL byte:
//
//   {
//     "version": 1,
//     "ignore_certificate_errors_spki_list": ["<SPKI hash>"],
//     "accept_language": "en-US,en;q=0.9",
//     "origins_to_force_quic_on": ["<host>:<port>"]
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
// May be called from any thread. No request may be running on the Session, and
// none may start afterwards. Does nothing if `session` is null.
MITTO_EXPORT void mitto_session_destroy(MittoSession* session);

// Performs a GET of `url`, a NUL-terminated UTF-8 string, on the library's
// network thread and blocks the calling thread until it finishes. May be called
// from any thread, also while other requests run on the same Session.
//
// `headers` points to `header_count` headers, in the order they are to reach
// the wire, and may be null if the count is 0. A name may appear only once,
// ignoring case. //net adds its own headers as it does in Chrome, such as Host
// before these and Accept-Encoding after them, unless they are among these.
// Referer cannot be among these, because //net only sends `referrer`.
//
// `priority` is one of the MITTO_PRIORITY values, and `priority_incremental`
// has the meaning of net::URLRequest::SetPriorityIncremental(). Together they
// decide the HTTP/2 weight, the priority header and the HTTP/3 PRIORITY_UPDATE.
//
// `referrer` is a NUL-terminated UTF-8 URL to send as the Referer header, or
// null to send none. //net puts it after `headers` and before the headers it
// adds, and leaves out any username, password and fragment. A URL that //net
// cannot send as a referrer, such as one that is not valid, is refused.
//
// Returns 0 on success and fills `response`, which the caller then passes to
// mitto_response_free(). An HTTP error status such as 404 counts as success. On
// failure, returns a negative Chromium net error code, such as -105 for
// ERR_NAME_NOT_RESOLVED or -4 (ERR_INVALID_ARGUMENT) for an invalid argument,
// and leaves `response` zeroed, so it needs no freeing.
MITTO_EXPORT int32_t mitto_session_request(MittoSession* session,
                                           const char* url,
                                           const MittoHeader* headers,
                                           size_t header_count,
                                           int32_t priority,
                                           bool priority_incremental,
                                           const char* referrer,
                                           MittoResponse* response);

// Releases the buffers of a response filled by mitto_session_request() and
// zeroes it. Does nothing if `response` is null or already zeroed.
MITTO_EXPORT void mitto_response_free(MittoResponse* response);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MITTO_MITTO_H_
