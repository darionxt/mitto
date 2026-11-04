// Fetches one URL through the native core and prints the result. Exits with 1
// if the library does not match this header or the request fails.
//
// Usage: mitto_core_test [url]

#include <inttypes.h>
#include <stdio.h>

#include "mitto/mitto.h"

int main(int argc, char** argv) {
  const char* url = argc > 1 ? argv[1] : "https://example.com/";

  if (mitto_abi_version() != MITTO_ABI_VERSION) {
    fprintf(stderr, "ABI version %" PRIu32 " does not match the header's %d\n",
            mitto_abi_version(), MITTO_ABI_VERSION);
    return 1;
  }

  MittoResponse response;
  const int32_t error = mitto_get(url, &response);
  if (error != 0) {
    fprintf(stderr, "GET %s failed with net error %" PRId32 "\n", url, error);
    return 1;
  }

  printf("status %" PRId32 " over %s\n", response.status, response.protocol);
  printf("%s", response.headers);
  printf("body: %zu bytes\n", response.body_size);

  mitto_response_free(&response);
  return 0;
}
