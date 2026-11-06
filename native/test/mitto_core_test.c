// Drives requests through the native core's C API, as an outside caller would,
// and prints what happens. Exits with 1 if the library does not match this
// header or something does not go as the scenario expects, and with 2 if it does
// not know the scenario.
//
// Usage: mitto_core_test [url [settings [scenario]]]
//
// `settings` is the JSON that mitto_session_create() reads. `scenario` is one of
// the names in kScenarios, and the default is the first of them.

#include <inttypes.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "mitto/mitto.h"

#define STRINGIFY_VALUE(value) #value
#define STRINGIFY(value) STRINGIFY_VALUE(value)

#define ERR_ABORTED (-3)

// What a scenario returns to have the process exit with everything still running.
#define EXIT_NOW (-1)

// How long a scenario waits for one event before it gives up, in milliseconds.
#define EVENT_TIMEOUT_MS (60 * 1000)

static const char kDefaultSettings[] =
    "{\"version\": " STRINGIFY(MITTO_SETTINGS_VERSION) "}";

static const char kOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"priority\": " STRINGIFY(MITTO_PRIORITY_MEDIUM) "}";

static const char kPostOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"method\": \"POST\"}";

// Each with one timer, for a server that accepts a connection and never answers.
static const char kConnectTimeoutOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"connect_timeout_ms\": 100}";
static const char kReadTimeoutOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"read_timeout_ms\": 100}";
static const char kTotalTimeoutOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"total_timeout_ms\": 100}";

static const char kCookies[] =
    "{\"version\": " STRINGIFY(MITTO_COOKIES_VERSION) ", \"cookies\": ["
    "{\"name\": \"added\", \"value\": \"1\", \"domain\": \".a.test\"}]}";
static const char kInvalidCookies[] =
    "{\"version\": " STRINGIFY(MITTO_COOKIES_VERSION) ", \"cookies\": ["
    "{\"name\": \"added\", \"value\": \"1\"}]}";
static const char kCookieFilter[] =
    "{\"version\": " STRINGIFY(MITTO_COOKIES_VERSION) ", \"name\": \"added\"}";

static const MittoHeader kHeaders[] = {
    {"Accept", sizeof("Accept") - 1, "*/*", sizeof("*/*") - 1},
};

// Global, so that what a scenario leaves running at exit is not a leak.
static MittoSession* g_session;
static MittoRequest* g_request;

static int32_t StartWithOptions(const char* url, const char* options) {
  const int32_t error = mitto_request_start(
      g_session, url, kHeaders, sizeof(kHeaders) / sizeof(kHeaders[0]), options,
      strlen(options), NULL, 0, &g_request);
  if (error != 0) {
    fprintf(stderr, "starting %s failed with net error %" PRId32 "\n", url,
            error);
  }
  return error;
}

static int32_t Start(const char* url) {
  return StartWithOptions(url, kOptions);
}

// Reads what has arrived, and asks for more once that is gone.
static void ReadBody(size_t* body_size) {
  static uint8_t buffer[64 * 1024];
  int32_t size;
  while ((size = mitto_request_read(g_request, buffer, sizeof(buffer))) > 0) {
    *body_size += (size_t)size;
  }
}

// Prints an event, and reads the body when an event says that it can. Returns
// whether the request has finished, and stores its net error if so.
static int HandleEvent(MittoEvent* event, size_t* body_size, int32_t* result) {
  switch (event->type) {
    case MITTO_EVENT_REDIRECT_FOLLOWED:
      printf("redirect %" PRId32 " to %s\n", event->status, event->url);
      break;
    case MITTO_EVENT_RESPONSE_STARTED:
      printf("status %" PRId32 " over %s\n", event->status, event->protocol);
      printf("url: %s\n", event->url);
      printf("%s", event->headers);
      ReadBody(body_size);
      break;
    case MITTO_EVENT_BODY_AVAILABLE:
      ReadBody(body_size);
      break;
    case MITTO_EVENT_FINISHED:
      *result = event->net_error;
      mitto_event_free(event);
      return 1;
  }
  mitto_event_free(event);
  return 0;
}

static int FinishFetch(const char* url, size_t body_size, int32_t result) {
  mitto_request_release(g_request);
  g_request = NULL;
  if (result != 0) {
    fprintf(stderr, "GET %s failed with net error %" PRId32 "\n", url, result);
    return 1;
  }
  printf("body: %zu bytes\n", body_size);
  return 0;
}

// Takes events in a blocking call, as a sync caller does.
static int FinishInBlockingCalls(const char* url) {
  size_t body_size = 0;
  int32_t result = 0;
  while (1) {
    MittoEvent event;
    const int32_t error = mitto_request_next_event(g_request, -1, &event);
    if (error != 0) {
      fprintf(stderr, "next event failed with net error %" PRId32 "\n", error);
      return 1;
    }
    if (HandleEvent(&event, &body_size, &result)) {
      return FinishFetch(url, body_size, result);
    }
  }
}

static int Fetch(const char* url) {
  if (Start(url) != 0) {
    return 1;
  }
  return FinishInBlockingCalls(url);
}

// Sends a body from a buffer that holds something else once the call has
// returned, as a caller that reuses its memory does.
static int Post(const char* url) {
  char body[] = "the body of a request";
  const size_t body_size = strlen(body);
  const int32_t error = mitto_request_start(
      g_session, url, kHeaders, sizeof(kHeaders) / sizeof(kHeaders[0]),
      kPostOptions, strlen(kPostOptions), (const uint8_t*)body, body_size,
      &g_request);
  if (error != 0) {
    fprintf(stderr, "starting %s failed with net error %" PRId32 "\n", url,
            error);
    return 1;
  }
  memset(body, 'x', body_size);
  return FinishInBlockingCalls(url);
}

// Waits on the file descriptor, as an event loop does.
static int FetchWithFileDescriptor(const char* url) {
  int32_t fd;
  int32_t error = mitto_session_event_fd(g_session, &fd);
  if (error != 0) {
    fprintf(stderr, "no file descriptor: net error %" PRId32 "\n", error);
    return 1;
  }
  if (Start(url) != 0) {
    return 1;
  }
  size_t body_size = 0;
  int32_t result = 0;
  while (1) {
    struct pollfd watched = {.fd = fd, .events = POLLIN};
    if (poll(&watched, 1, EVENT_TIMEOUT_MS) != 1) {
      fprintf(stderr, "the file descriptor did not become readable\n");
      return 1;
    }
    MittoRequest* ready;
    error = mitto_session_next_ready_request(g_session, &ready);
    if (error != 0 || ready != g_request) {
      fprintf(stderr, "no request is ready: net error %" PRId32 "\n", error);
      return 1;
    }
    MittoEvent event;
    error = mitto_request_next_event(ready, 0, &event);
    if (error != 0) {
      fprintf(stderr, "next event failed with net error %" PRId32 "\n", error);
      return 1;
    }
    if (HandleEvent(&event, &body_size, &result)) {
      return FinishFetch(url, body_size, result);
    }
  }
}

// Waits for the event of `type`, leaving the body unread. Returns 0 if it came.
static int WaitFor(int32_t type) {
  while (1) {
    MittoEvent event;
    const int32_t error =
        mitto_request_next_event(g_request, EVENT_TIMEOUT_MS, &event);
    if (error != 0) {
      fprintf(stderr, "next event failed with net error %" PRId32 "\n", error);
      return 1;
    }
    const int32_t event_type = event.type;
    mitto_event_free(&event);
    if (event_type == type) {
      return 0;
    }
    if (event_type == MITTO_EVENT_FINISHED) {
      fprintf(stderr, "the request finished too early\n");
      return 1;
    }
  }
}

// Checks that the request ends as a cancelled one does, and releases it.
static int ExpectCancelled(void) {
  while (1) {
    MittoEvent event;
    const int32_t error =
        mitto_request_next_event(g_request, EVENT_TIMEOUT_MS, &event);
    if (error != 0) {
      fprintf(stderr, "next event failed with net error %" PRId32 "\n", error);
      return 1;
    }
    const int32_t event_type = event.type;
    const int32_t net_error = event.net_error;
    mitto_event_free(&event);
    if (event_type == MITTO_EVENT_FINISHED) {
      mitto_request_release(g_request);
      g_request = NULL;
      if (net_error != ERR_ABORTED) {
        fprintf(stderr, "finished with net error %" PRId32 "\n", net_error);
        return 1;
      }
      printf("cancelled\n");
      return 0;
    }
  }
}

static int CancelAtOnce(const char* url) {
  if (Start(url) != 0) {
    return 1;
  }
  mitto_request_cancel(g_request);
  return ExpectCancelled() || Fetch(url);
}

static int CancelAfterResponse(const char* url) {
  if (Start(url) != 0 || WaitFor(MITTO_EVENT_RESPONSE_STARTED) != 0) {
    return 1;
  }
  mitto_request_cancel(g_request);
  return ExpectCancelled() || Fetch(url);
}

static int ReleaseAtOnce(const char* url) {
  if (Start(url) != 0) {
    return 1;
  }
  mitto_request_release(g_request);
  g_request = NULL;
  return Fetch(url);
}

static int ReleaseAfterResponse(const char* url) {
  if (Start(url) != 0 || WaitFor(MITTO_EVENT_RESPONSE_STARTED) != 0) {
    return 1;
  }
  mitto_request_release(g_request);
  g_request = NULL;
  return Fetch(url);
}

// main() destroys the Session after a scenario, which does nothing the second
// time.
static int DestroySessionWhileRunning(const char* url) {
  if (Start(url) != 0) {
    return 1;
  }
  mitto_session_destroy(g_session);
  g_session = NULL;
  return ExpectCancelled();
}

// Leaves the request running, with its response not read, for the process to
// exit with.
static int ExitWhileRunning(const char* url) {
  if (Start(url) != 0 || WaitFor(MITTO_EVENT_RESPONSE_STARTED) != 0) {
    return 1;
  }
  printf("exiting\n");
  return EXIT_NOW;
}

// Starts a request that a timer has to end, and prints how it ended.
static int ExpectTimeout(const char* url, const char* options) {
  if (StartWithOptions(url, options) != 0) {
    return 1;
  }
  while (1) {
    MittoEvent event;
    const int32_t error =
        mitto_request_next_event(g_request, EVENT_TIMEOUT_MS, &event);
    if (error != 0) {
      fprintf(stderr, "next event failed with net error %" PRId32 "\n", error);
      return 1;
    }
    const int32_t event_type = event.type;
    const int32_t net_error = event.net_error;
    mitto_event_free(&event);
    if (event_type == MITTO_EVENT_FINISHED) {
      printf("timed out with %s\n", mitto_net_error_name(net_error));
      return 0;
    }
  }
}

static int ConnectTimeout(const char* url) {
  return ExpectTimeout(url, kConnectTimeoutOptions);
}

static int ReadTimeout(const char* url) {
  return ExpectTimeout(url, kReadTimeoutOptions);
}

static int TotalTimeout(const char* url) {
  return ExpectTimeout(url, kTotalTimeoutOptions);
}

// Waits for a byte on stdin. The caller sends one when the request has reached a
// point that no event shows, such as a certificate fetch. Returns 0 if it came.
static int WaitToBeTold(void) {
  return getchar() == EOF;
}

static int DestroySessionWhenTold(const char* url) {
  if (Start(url) != 0 || WaitToBeTold() != 0) {
    return 1;
  }
  mitto_session_destroy(g_session);
  g_session = NULL;
  return ExpectCancelled();
}

static int ExitWhenTold(const char* url) {
  if (Start(url) != 0 || WaitToBeTold() != 0) {
    return 1;
  }
  printf("exiting\n");
  return EXIT_NOW;
}

static int PrintCookies(void) {
  char* cookies;
  size_t size;
  const int32_t error = mitto_session_list_cookies(g_session, &cookies, &size);
  if (error != 0) {
    fprintf(stderr, "listing cookies failed with net error %" PRId32 "\n",
            error);
    return 1;
  }
  printf("cookies, %zu bytes: %s\n", size, cookies);
  mitto_cookies_free(cookies);
  return 0;
}

// Lists what a response has set, and then adds and deletes a cookie.
static int Cookies(const char* url) {
  if (Fetch(url) != 0 || PrintCookies() != 0) {
    return 1;
  }
  int32_t error =
      mitto_session_set_cookies(g_session, kCookies, strlen(kCookies));
  if (error != 0) {
    fprintf(stderr, "setting a cookie failed with net error %" PRId32 "\n",
            error);
    return 1;
  }
  if (PrintCookies() != 0) {
    return 1;
  }
  uint32_t deleted;
  error = mitto_session_delete_cookies(g_session, kCookieFilter,
                                       strlen(kCookieFilter), &deleted);
  if (error != 0) {
    fprintf(stderr, "deleting a cookie failed with net error %" PRId32 "\n",
            error);
    return 1;
  }
  printf("deleted %" PRIu32 "\n", deleted);
  if (mitto_session_set_cookies(g_session, kInvalidCookies,
                                strlen(kInvalidCookies)) == 0) {
    fprintf(stderr, "a cookie with no domain was set\n");
    return 1;
  }
  return PrintCookies();
}

static const struct {
  const char* name;
  int (*run)(const char* url);
} kScenarios[] = {
    {"fetch", Fetch},
    {"post", Post},
    {"fetch-with-file-descriptor", FetchWithFileDescriptor},
    {"cancel-at-once", CancelAtOnce},
    {"cancel-after-response", CancelAfterResponse},
    {"release-at-once", ReleaseAtOnce},
    {"release-after-response", ReleaseAfterResponse},
    {"destroy-session-while-running", DestroySessionWhileRunning},
    {"exit-while-running", ExitWhileRunning},
    {"connect-timeout", ConnectTimeout},
    {"read-timeout", ReadTimeout},
    {"total-timeout", TotalTimeout},
    {"destroy-session-when-told", DestroySessionWhenTold},
    {"exit-when-told", ExitWhenTold},
    {"cookies", Cookies},
};

int main(int argc, char** argv) {
  const char* url = argc > 1 ? argv[1] : "https://example.com/";
  const char* settings = argc > 2 ? argv[2] : kDefaultSettings;
  const char* scenario = argc > 3 ? argv[3] : kScenarios[0].name;

  int (*run)(const char* url) = NULL;
  for (size_t i = 0; i < sizeof(kScenarios) / sizeof(kScenarios[0]); ++i) {
    if (strcmp(kScenarios[i].name, scenario) == 0) {
      run = kScenarios[i].run;
    }
  }
  if (!run) {
    fprintf(stderr, "unknown scenario %s\n", scenario);
    return 2;
  }

  if (mitto_abi_version() != MITTO_ABI_VERSION) {
    fprintf(stderr, "ABI version %" PRIu32 " does not match the header's %d\n",
            mitto_abi_version(), MITTO_ABI_VERSION);
    return 1;
  }

  const int32_t error =
      mitto_session_create(settings, strlen(settings), &g_session);
  if (error != 0) {
    fprintf(stderr, "creating a Session failed with net error %" PRId32 "\n",
            error);
    return 1;
  }

  const int result = run(url);
  if (result == EXIT_NOW) {
    return 0;
  }
  mitto_request_release(g_request);
  mitto_session_destroy(g_session);
  return result;
}
