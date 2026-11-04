// The native core's public C API, the contract between the native core and its
// callers, such as the Python package. Increase MITTO_ABI_VERSION on any
// incompatible change, including a change to the layout of MittoResponse.
//
// Once loaded, the library stays in memory until the process exits, because its
// network thread never stops. dlclose() does not unload it.

#ifndef MITTO_MITTO_H_
#define MITTO_MITTO_H_

#include <stddef.h>
#include <stdint.h>

#define MITTO_ABI_VERSION 1

#define MITTO_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

// A finished response. mitto_get() allocates the buffers and the caller
// releases them with mitto_response_free(). Each buffer ends with a NUL byte
// that its size does not count.
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

// Performs a GET of `url`, a NUL-terminated UTF-8 string, on the library's
// network thread and blocks the calling thread until it finishes. May be called
// from any thread.
//
// Returns 0 on success and fills `response`, which the caller then passes to
// mitto_response_free(). An HTTP error status such as 404 counts as success. On
// failure, returns a negative Chromium net error code, such as -105 for
// ERR_NAME_NOT_RESOLVED, and leaves `response` zeroed, so it needs no freeing.
MITTO_EXPORT int32_t mitto_get(const char* url, MittoResponse* response);

// Releases the buffers of a response filled by mitto_get() and zeroes it. Does
// nothing if `response` is null or already zeroed.
MITTO_EXPORT void mitto_response_free(MittoResponse* response);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MITTO_MITTO_H_
