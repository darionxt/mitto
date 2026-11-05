// Fetches one URL through the native core and prints the result. Exits with 1
// if the library does not match this header or the request fails.
//
// Usage: mitto_core_test [url]

#include <inttypes.h>
#include <stdio.h>

#include "mitto/mitto.h"

#define STRINGIFY_VALUE(value) #value
#define STRINGIFY(value) STRINGIFY_VALUE(value)

static const char kSettings[] =
    "{\"version\": " STRINGIFY(MITTO_SETTINGS_VERSION) "}";

static const MittoHeader kHeaders[] = {
    {"Accept", sizeof("Accept") - 1, "*/*", sizeof("*/*") - 1},
};

int main(int argc, char** argv) {
  const char* url = argc > 1 ? argv[1] : "https://example.com/";

  if (mitto_abi_version() != MITTO_ABI_VERSION) {
    fprintf(stderr, "ABI version %" PRIu32 " does not match the header's %d\n",
            mitto_abi_version(), MITTO_ABI_VERSION);
    return 1;
  }

  MittoSession* session;
  int32_t error =
      mitto_session_create(kSettings, sizeof(kSettings) - 1, &session);
  if (error != 0) {
    fprintf(stderr, "creating a Session failed with net error %" PRId32 "\n",
            error);
    return 1;
  }

  MittoResponse response;
  error = mitto_session_request(session, url, kHeaders,
                                sizeof(kHeaders) / sizeof(kHeaders[0]),
                                MITTO_PRIORITY_MEDIUM, &response);
  if (error != 0) {
    fprintf(stderr, "GET %s failed with net error %" PRId32 "\n", url, error);
    mitto_session_destroy(session);
    return 1;
  }

  printf("status %" PRId32 " over %s\n", response.status, response.protocol);
  printf("%s", response.headers);
  printf("body: %zu bytes\n", response.body_size);

  mitto_response_free(&response);
  mitto_session_destroy(session);
  return 0;
}
