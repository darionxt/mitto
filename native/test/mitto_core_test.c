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
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "mitto/mitto.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define STRINGIFY_VALUE(value) #value
#define STRINGIFY(value) STRINGIFY_VALUE(value)

#define ERR_IO_PENDING (-1)
#define ERR_ABORTED (-3)
#define ERR_NOT_IMPLEMENTED (-11)
#define ERR_UPLOAD_FILE_CHANGED (-14)

// What a scenario returns to have the process exit with everything still running.
#define EXIT_NOW (-1)

// How long a scenario waits for one event before it gives up, in milliseconds.
#define EVENT_TIMEOUT_MS (60 * 1000)

static const char kDefaultSettings[] =
    "{\"version\": " STRINGIFY(MITTO_SETTINGS_VERSION) "}";

static const char kOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"priority\": " STRINGIFY(MITTO_PRIORITY_MEDIUM) "}";

static const char kNoRedirectOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"follow_redirects\": false}";

static const char kPostOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"method\": \"POST\"}";

// What a Persona's request says of itself, so that the rules of each hop run.
static const char kNavigationFromAPageOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"fetch\": {\"mode\": \"navigate\", \"destination\": \"document\", "
    "\"credentials_mode\": \"include\", \"has_user_activation\": true, "
    "\"initiator\": \"https://a.test/page\"}}";
static const char kCorsFromAPageOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"fetch\": {\"mode\": \"cors\", \"destination\": \"empty\", "
    "\"credentials_mode\": \"same-origin\", "
    "\"initiator\": \"https://a.test/page\", "
    "\"fixed_headers\": [\"Accept\"]}}";
static const char kNoCorsWithNoInitiatorOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"fetch\": {\"mode\": \"no-cors\", \"destination\": \"script\", "
    "\"credentials_mode\": \"include\"}}";

static const char kStreamOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"method\": \"POST\", \"stream_body\": true}";
static const char kSizedStreamOptions[] =
    "{\"version\": " STRINGIFY(MITTO_REQUEST_OPTIONS_VERSION) ", "
    "\"method\": \"POST\", \"stream_body\": true, "
    "\"stream_body_size\": 21}";

// What a stream sends, a few bytes at a time. kSizedStreamOptions has its size.
static const char kStreamBody[] = "the body of a request";
#define STREAM_PIECE_SIZE 8

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

// With the lines that the rules of each hop set, move and remove.
static const MittoHeader kHeadersOfAPersona[] = {
    {"sec-ch-ua", sizeof("sec-ch-ua") - 1, "\"a\"", sizeof("\"a\"") - 1},
    {"Accept", sizeof("Accept") - 1, "*/*", sizeof("*/*") - 1},
    {"Origin", sizeof("Origin") - 1, "<origin>", sizeof("<origin>") - 1},
    {"Sec-Fetch-Site", sizeof("Sec-Fetch-Site") - 1, "none", sizeof("none") - 1},
    {"Sec-Fetch-Mode", sizeof("Sec-Fetch-Mode") - 1, "cors", sizeof("cors") - 1},
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
      printf("redirect %" PRId32 " from %s\n", event->status, event->url);
      break;
    case MITTO_EVENT_RESPONSE_STARTED:
      printf("status %" PRId32 " over %s\n", event->status, event->protocol);
      if (event->from_cache) {
        printf("from the cache\n");
      }
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

// Asks for the same URL again, which a Session with a cache can answer itself.
static int FetchTwice(const char* url) {
  if (Fetch(url) != 0) {
    return 1;
  }
  return Fetch(url);
}

// A redirect is the response here, with no body.
static int FetchWithoutRedirects(const char* url) {
  if (StartWithOptions(url, kNoRedirectOptions) != 0) {
    return 1;
  }
  return FinishInBlockingCalls(url);
}

// Makes a request with what a Persona gives it, and goes through what follows.
static int FetchLikeAPersona(const char* url, const char* options) {
  const int32_t error = mitto_request_start(
      g_session, url, kHeadersOfAPersona,
      sizeof(kHeadersOfAPersona) / sizeof(kHeadersOfAPersona[0]), options,
      strlen(options), NULL, 0, &g_request);
  if (error != 0) {
    fprintf(stderr, "starting %s failed with net error %" PRId32 "\n", url,
            error);
    return 1;
  }
  return FinishInBlockingCalls(url);
}

static int NavigationFromAPage(const char* url) {
  return FetchLikeAPersona(url, kNavigationFromAPageOptions);
}

static int CorsFromAPage(const char* url) {
  return FetchLikeAPersona(url, kCorsFromAPageOptions);
}

static int NoCorsWithNoInitiator(const char* url) {
  return FetchLikeAPersona(url, kNoCorsWithNoInitiatorOptions);
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

// Writes the next piece of a stream, or ends it if none is left.
static int SendNextPiece(size_t* sent) {
  const size_t left = strlen(kStreamBody) - *sent;
  if (left == 0) {
    const int32_t error = mitto_request_end_body(g_request);
    if (error != 0) {
      fprintf(stderr, "ending a body failed with net error %" PRId32 "\n", error);
      return 1;
    }
    return 0;
  }
  const size_t size = left < STREAM_PIECE_SIZE ? left : STREAM_PIECE_SIZE;
  const int32_t written = mitto_request_write_body(
      g_request, (const uint8_t*)kStreamBody + *sent, size);
  if (written < 0) {
    fprintf(stderr, "writing a body failed with net error %" PRId32 "\n",
            written);
    return 1;
  }
  *sent += (size_t)written;
  return 0;
}

static int PostStreamWithOptions(const char* url, const char* options) {
  if (StartWithOptions(url, options) != 0) {
    return 1;
  }
  size_t sent = 0;
  size_t body_size = 0;
  int32_t result = 0;
  while (1) {
    MittoEvent event;
    const int32_t error = mitto_request_next_event(g_request, -1, &event);
    if (error != 0) {
      fprintf(stderr, "next event failed with net error %" PRId32 "\n", error);
      return 1;
    }
    if (event.type == MITTO_EVENT_BODY_WANTED) {
      mitto_event_free(&event);
      if (SendNextPiece(&sent) != 0) {
        return 1;
      }
      continue;
    }
    if (HandleEvent(&event, &body_size, &result)) {
      printf("sent: %zu bytes\n", sent);
      return FinishFetch(url, body_size, result);
    }
  }
}

static int PostStream(const char* url) {
  return PostStreamWithOptions(url, kStreamOptions);
}

static int PostSizedStream(const char* url) {
  return PostStreamWithOptions(url, kSizedStreamOptions);
}

#if defined(_WIN32)
// A Session has no file descriptor on Windows, so this checks that the call
// says so, and then fetches as Fetch() does.
static int FetchWithFileDescriptor(const char* url) {
  int32_t fd;
  const int32_t error = mitto_session_event_fd(g_session, &fd);
  if (error != ERR_NOT_IMPLEMENTED || fd != -1) {
    fprintf(stderr, "a file descriptor on Windows: net error %" PRId32 "\n",
            error);
    return 1;
  }
  return Fetch(url);
}
#else
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
    int32_t kind;
    void* handle;
    error = mitto_session_next_ready(g_session, &kind, &handle);
    if (error != 0 || kind != MITTO_READY_REQUEST || handle != g_request) {
      fprintf(stderr, "no request is ready: net error %" PRId32 "\n", error);
      return 1;
    }
    MittoRequest* ready = handle;
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
#endif

// Waits in mitto_session_wait_for_event(), as a thread of its own does for an
// event loop on Windows, and takes every event it says is waiting before it
// waits again.
static int FetchWithWaits(const char* url) {
  int32_t error = mitto_session_wait_for_event(g_session, 0);
  if (error != ERR_IO_PENDING) {
    fprintf(stderr, "a first wait gave net error %" PRId32 "\n", error);
    return 1;
  }
  if (Start(url) != 0) {
    return 1;
  }
  size_t body_size = 0;
  int32_t result = 0;
  while (1) {
    error = mitto_session_wait_for_event(g_session, EVENT_TIMEOUT_MS);
    if (error != 0) {
      fprintf(stderr, "no event came: net error %" PRId32 "\n", error);
      return 1;
    }
    int32_t kind;
    void* handle;
    while (mitto_session_next_ready(g_session, &kind, &handle) == 0) {
      if (kind != MITTO_READY_REQUEST || handle != g_request) {
        fprintf(stderr, "another request is ready\n");
        return 1;
      }
      MittoRequest* ready = handle;
      MittoEvent event;
      error = mitto_request_next_event(ready, 0, &event);
      if (error != 0) {
        fprintf(stderr, "next event failed with net error %" PRId32 "\n",
                error);
        return 1;
      }
      if (HandleEvent(&event, &body_size, &result)) {
        mitto_session_end_waits(g_session);
        error = mitto_session_wait_for_event(g_session, -1);
        if (error != ERR_ABORTED) {
          fprintf(stderr, "a wait after the end gave net error %" PRId32 "\n",
                  error);
          return 1;
        }
        return FinishFetch(url, body_size, result);
      }
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

// Waits for the request's last event, and stores its net error.
static int WaitForFinished(int32_t* net_error) {
  while (1) {
    MittoEvent event;
    const int32_t error =
        mitto_request_next_event(g_request, EVENT_TIMEOUT_MS, &event);
    if (error != 0) {
      fprintf(stderr, "next event failed with net error %" PRId32 "\n", error);
      return 1;
    }
    const int32_t event_type = event.type;
    *net_error = event.net_error;
    mitto_event_free(&event);
    if (event_type == MITTO_EVENT_FINISHED) {
      return 0;
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

static int StartStreamAndWaitForIt(const char* url, const char* options) {
  if (StartWithOptions(url, options) != 0) {
    return 1;
  }
  return WaitFor(MITTO_EVENT_BODY_WANTED);
}

// A stream takes nothing more once its request has ended.
static int CancelStreamWhileItWaits(const char* url) {
  if (StartStreamAndWaitForIt(url, kStreamOptions) != 0) {
    return 1;
  }
  mitto_request_cancel(g_request);
  int32_t net_error = 0;
  if (WaitForFinished(&net_error) != 0) {
    return 1;
  }
  const int32_t written =
      mitto_request_write_body(g_request, (const uint8_t*)kStreamBody, 1);
  const int32_t ended = mitto_request_end_body(g_request);
  if (net_error != ERR_ABORTED || written != 0 || ended != 0) {
    fprintf(stderr,
            "finished with %" PRId32 ", wrote %" PRId32 ", ended with %" PRId32
            "\n",
            net_error, written, ended);
    return 1;
  }
  printf("cancelled\n");
  return 0;
}

static int ReleaseStreamWhileItWaits(const char* url) {
  if (StartStreamAndWaitForIt(url, kStreamOptions) != 0) {
    return 1;
  }
  mitto_request_release(g_request);
  g_request = NULL;
  printf("released\n");
  return Fetch(url);
}

static int DestroySessionWhileStreamWaits(const char* url) {
  if (StartStreamAndWaitForIt(url, kStreamOptions) != 0) {
    return 1;
  }
  mitto_session_destroy(g_session);
  g_session = NULL;
  return ExpectCancelled();
}

// A write takes as much as it can before anything has asked for it.
static int WriteStreamBeforeItIsWanted(const char* url) {
  if (StartWithOptions(url, kStreamOptions) != 0) {
    return 1;
  }
  size_t sent = 0;
  if (SendNextPiece(&sent) != 0) {
    return 1;
  }
  const int32_t refused = mitto_request_write_body(
      g_request, (const uint8_t*)kStreamBody + sent, 1);
  if (refused != -1) {
    fprintf(stderr, "a second write gave %" PRId32 "\n", refused);
    return 1;
  }
  size_t body_size = 0;
  int32_t result = 0;
  while (1) {
    MittoEvent event;
    const int32_t error = mitto_request_next_event(g_request, -1, &event);
    if (error != 0) {
      fprintf(stderr, "next event failed with net error %" PRId32 "\n", error);
      return 1;
    }
    if (event.type == MITTO_EVENT_BODY_WANTED) {
      mitto_event_free(&event);
      if (SendNextPiece(&sent) != 0) {
        return 1;
      }
      continue;
    }
    if (HandleEvent(&event, &body_size, &result)) {
      printf("sent: %zu bytes\n", sent);
      return FinishFetch(url, body_size, result);
    }
  }
}

// A body that ends before its size fails the request.
static int EndStreamBeforeItsSize(const char* url) {
  if (StartStreamAndWaitForIt(url, kSizedStreamOptions) != 0) {
    return 1;
  }
  const int32_t error = mitto_request_end_body(g_request);
  int32_t net_error = 0;
  if (error != 0 || WaitForFinished(&net_error) != 0) {
    return 1;
  }
  if (net_error != ERR_UPLOAD_FILE_CHANGED) {
    fprintf(stderr, "finished with net error %" PRId32 "\n", net_error);
    return 1;
  }
  printf("too short\n");
  return 0;
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

// Takes events until the last one and releases the request. A request that was
// cancelled while it was on its way may finish all the same.
static int DrainToEnd(void) {
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
    if (event_type == MITTO_EVENT_FINISHED) {
      mitto_request_release(g_request);
      g_request = NULL;
      printf("ended\n");
      return 0;
    }
  }
}

static int CancelAtRedirect(const char* url) {
  if (Start(url) != 0 || WaitFor(MITTO_EVENT_REDIRECT_FOLLOWED) != 0) {
    return 1;
  }
  mitto_request_cancel(g_request);
  return DrainToEnd() || Fetch(url);
}

// main() destroys the Session after a scenario, which does nothing the second
// time.
static int DestroySessionAtRedirect(const char* url) {
  if (Start(url) != 0 || WaitFor(MITTO_EVENT_REDIRECT_FOLLOWED) != 0) {
    return 1;
  }
  mitto_session_destroy(g_session);
  g_session = NULL;
  return DrainToEnd();
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

// The scenarios of Pages. A Page's process runs the Page program that lies
// beside the library.

// What a Page starts with, as a Session's Machine and Persona say it.
static const char kPageOptions[] =
    "{\"version\": 2, \"start\": {\"time_zone\": \"America/New_York\", "
    "\"locale\": \"en-US\", \"processors\": 8, \"touch_points\": 0, "
    "\"work_area_x\": 0, \"work_area_y\": 0, \"work_area_width\": 1920, "
    "\"work_area_height\": 1032, \"window_state\": \"maximised\", "
    "\"window_x\": 0, \"window_y\": 0, \"window_width\": 1920, "
    "\"window_height\": 1032, \"contrast_theme\": \"none\", "
    "\"inverted_colors\": \"none\"}}";

static MittoPage* g_page;
static int64_t g_page_process;
// The Page's MITTO_PAGE_EVENT_ENDED event has been printed.
static int g_page_ended;

#if defined(_WIN32)
static int ProcessIsGone(int64_t process_id) {
  HANDLE process =
      OpenProcess(SYNCHRONIZE, FALSE, (DWORD)process_id);
  if (!process) {
    return GetLastError() == ERROR_INVALID_PARAMETER;
  }
  const int gone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
  CloseHandle(process);
  return gone;
}

static int KillProcess(int64_t process_id) {
  HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)process_id);
  if (!process) {
    return 1;
  }
  const int killed = TerminateProcess(process, 9);
  CloseHandle(process);
  return !killed;
}

static void SleepForever(void) {
  Sleep(INFINITE);
}
#else
static int ProcessIsGone(int64_t process_id) {
  return kill((pid_t)process_id, 0) != 0 && errno == ESRCH;
}

static int KillProcess(int64_t process_id) {
  return kill((pid_t)process_id, SIGKILL) != 0;
}

static void SleepForever(void) {
  while (1) {
    pause();
  }
}
#endif

static const char* PageErrorName(int32_t error) {
  switch (error) {
    case MITTO_PAGE_ERROR_NONE:
      return "none";
    case MITTO_PAGE_ERROR_SCRIPT:
      return "script";
    case MITTO_PAGE_ERROR_PROCESS_ENDED:
      return "process ended";
    case MITTO_PAGE_ERROR_REFUSED:
      return "refused";
    case MITTO_PAGE_ERROR_SESSION_DESTROYED:
      return "session destroyed";
    default:
      return "unknown";
  }
}

static int OpenPage(void) {
  const int32_t error =
      mitto_page_open(g_session, kPageOptions, strlen(kPageOptions), &g_page);
  if (error != 0) {
    fprintf(stderr, "opening a Page failed with net error %" PRId32 "\n",
            error);
    return 1;
  }
  if (mitto_page_process_id(g_page, &g_page_process) != 0 ||
      g_page_process <= 0) {
    fprintf(stderr, "the Page has no process\n");
    return 1;
  }
  return 0;
}

static int Evaluate(const char* script, uint64_t* call) {
  const int32_t error =
      mitto_page_evaluate(g_page, script, strlen(script), call);
  if (error != 0) {
    fprintf(stderr, "evaluating failed with net error %" PRId32 "\n", error);
    return 1;
  }
  return 0;
}

// Takes the Page's next event into `event`, which the caller frees.
static int NextPageEvent(MittoPageEvent* event) {
  const int32_t error = mitto_page_next_event(g_page, EVENT_TIMEOUT_MS, event);
  if (error != 0) {
    fprintf(stderr, "no event of the Page came: net error %" PRId32 "\n",
            error);
    return 1;
  }
  return 0;
}

// Waits for the result of `call` and prints it.
static int PrintResult(uint64_t call) {
  while (1) {
    MittoPageEvent event;
    if (NextPageEvent(&event) != 0) {
      return 1;
    }
    const int done = event.type == MITTO_PAGE_EVENT_EVALUATED &&
                     event.call == call;
    if (done && event.error == MITTO_PAGE_ERROR_NONE) {
      printf("evaluated %s\n", event.json);
    } else if (done) {
      printf("failed, %s: %s\n", PageErrorName(event.error), event.message);
    } else if (event.type == MITTO_PAGE_EVENT_ENDED) {
      printf("ended, %s: %s\n", PageErrorName(event.error), event.message);
      g_page_ended = 1;
    }
    mitto_page_event_free(&event);
    if (done) {
      return 0;
    }
  }
}

// Waits for the Page to end, unless it has, and prints why.
static int PrintEnded(void) {
  while (!g_page_ended) {
    MittoPageEvent event;
    if (NextPageEvent(&event) != 0) {
      return 1;
    }
    const int ended = event.type == MITTO_PAGE_EVENT_ENDED;
    if (event.type == MITTO_PAGE_EVENT_EVALUATED) {
      printf("call %" PRIu64 " failed, %s: %s\n", event.call,
             PageErrorName(event.error),
             event.message ? event.message : "");
    } else if (ended) {
      printf("ended, %s: %s\n", PageErrorName(event.error), event.message);
      g_page_ended = 1;
    }
    mitto_page_event_free(&event);
  }
  return 0;
}

static int ClosePage(void) {
  mitto_page_close(g_page);
  g_page = NULL;
  if (!ProcessIsGone(g_page_process)) {
    fprintf(stderr, "the Page process is still there\n");
    return 1;
  }
  printf("the Page process is gone\n");
  return 0;
}

// Opens a Page, runs 1 + 1 in it, finds the result through the Session's list
// of what is ready, closes the Page and checks that its process is gone.
static int Page(const char* url) {
  // From now on the Session lists what is ready.
  int32_t error = mitto_session_wait_for_event(g_session, 0);
  if (error != ERR_IO_PENDING) {
    fprintf(stderr, "a first wait gave net error %" PRId32 "\n", error);
    return 1;
  }
  uint64_t call;
  if (OpenPage() != 0 || Evaluate("1 + 1", &call) != 0) {
    return 1;
  }
  error = mitto_session_wait_for_event(g_session, EVENT_TIMEOUT_MS);
  int32_t kind;
  void* handle;
  if (error != 0 || mitto_session_next_ready(g_session, &kind, &handle) != 0 ||
      kind != MITTO_READY_PAGE || handle != g_page) {
    fprintf(stderr, "the Page is not ready: net error %" PRId32 "\n", error);
    return 1;
  }
  if (PrintResult(call) != 0) {
    return 1;
  }
  return ClosePage();
}

// Kills the Page process, prints how its calls and the Page end, and then
// fetches `url` on the same Session.
static int PageKill(const char* url) {
  uint64_t call;
  if (OpenPage() != 0 || Evaluate("1 + 1", &call) != 0 ||
      PrintResult(call) != 0) {
    return 1;
  }
  if (KillProcess(g_page_process) != 0) {
    fprintf(stderr, "the Page process could not be killed\n");
    return 1;
  }
  // Whether this call is in progress when the library notices or comes after,
  // it fails the same way.
  if (Evaluate("2 + 2", &call) != 0 || PrintResult(call) != 0 ||
      PrintEnded() != 0) {
    return 1;
  }
  if (Evaluate("3 + 3", &call) != 0 || PrintResult(call) != 0) {
    return 1;
  }
  if (ClosePage() != 0) {
    return 1;
  }
  return Fetch(url);
}

// Prints the Page's process ID and waits to be ended.
static int PageHold(const char* url) {
  uint64_t call;
  if (OpenPage() != 0 || Evaluate("1 + 1", &call) != 0 ||
      PrintResult(call) != 0) {
    return 1;
  }
  printf("page process %" PRId64 "\n", g_page_process);
  fflush(stdout);
  SleepForever();
  return 1;
}

#if !defined(_WIN32)
// As PageHold, but first forks a child that holds this process's end of the
// Page's channel and outlives it, so that the Page reads no end of the stream
// when this process is killed.
static int PageHoldForked(const char* url) {
  uint64_t call;
  if (OpenPage() != 0 || Evaluate("1 + 1", &call) != 0 ||
      PrintResult(call) != 0) {
    return 1;
  }
  const pid_t child = fork();
  if (child < 0) {
    return 1;
  }
  if (child == 0) {
    // Ends when the test kills it, and holds every descriptor until then.
    while (1) {
      pause();
    }
  }
  printf("fork %" PRId64 "\n", (int64_t)child);
  printf("page process %" PRId64 "\n", g_page_process);
  fflush(stdout);
  SleepForever();
  return 1;
}
#endif

// Opens a Page and prints how its first call ends, for a Page program that
// refuses or is refused.
static int PageFirstCall(const char* url) {
  uint64_t call;
  if (OpenPage() != 0 || Evaluate("1 + 1", &call) != 0 ||
      PrintResult(call) != 0 || PrintEnded() != 0) {
    return 1;
  }
  return ClosePage();
}

// Destroys the Session while its Page is open, and prints how the Page ends.
static int PageDestroySession(const char* url) {
  uint64_t call;
  if (OpenPage() != 0 || Evaluate("1 + 1", &call) != 0 ||
      PrintResult(call) != 0) {
    return 1;
  }
  mitto_session_destroy(g_session);
  g_session = NULL;
  if (!ProcessIsGone(g_page_process)) {
    fprintf(stderr, "the Page process outlived its Session\n");
    return 1;
  }
  if (PrintEnded() != 0 || Evaluate("2 + 2", &call) != 0 ||
      PrintResult(call) != 0) {
    return 1;
  }
  mitto_page_close(g_page);
  g_page = NULL;
  printf("closed\n");
  return 0;
}

static const struct {
  const char* name;
  int (*run)(const char* url);
} kScenarios[] = {
    {"fetch", Fetch},
    {"fetch-twice", FetchTwice},
    {"post", Post},
    {"post-stream", PostStream},
    {"post-sized-stream", PostSizedStream},
    {"cancel-stream-while-it-waits", CancelStreamWhileItWaits},
    {"release-stream-while-it-waits", ReleaseStreamWhileItWaits},
    {"destroy-session-while-stream-waits", DestroySessionWhileStreamWaits},
    {"write-stream-before-it-is-wanted", WriteStreamBeforeItIsWanted},
    {"end-stream-before-its-size", EndStreamBeforeItsSize},
    {"fetch-with-file-descriptor", FetchWithFileDescriptor},
    {"fetch-with-waits", FetchWithWaits},
    {"fetch-without-redirects", FetchWithoutRedirects},
    {"navigation-from-a-page", NavigationFromAPage},
    {"cors-from-a-page", CorsFromAPage},
    {"no-cors-with-no-initiator", NoCorsWithNoInitiator},
    {"cancel-at-once", CancelAtOnce},
    {"cancel-after-response", CancelAfterResponse},
    {"cancel-at-redirect", CancelAtRedirect},
    {"release-at-once", ReleaseAtOnce},
    {"release-after-response", ReleaseAfterResponse},
    {"destroy-session-while-running", DestroySessionWhileRunning},
    {"destroy-session-at-redirect", DestroySessionAtRedirect},
    {"exit-while-running", ExitWhileRunning},
    {"connect-timeout", ConnectTimeout},
    {"read-timeout", ReadTimeout},
    {"total-timeout", TotalTimeout},
    {"destroy-session-when-told", DestroySessionWhenTold},
    {"exit-when-told", ExitWhenTold},
    {"cookies", Cookies},
    {"page", Page},
    {"page-kill", PageKill},
    {"page-hold", PageHold},
#if !defined(_WIN32)
    {"page-hold-forked", PageHoldForked},
#endif
    {"page-first-call", PageFirstCall},
    {"page-destroy-session", PageDestroySession},
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
  mitto_page_close(g_page);
  mitto_request_release(g_request);
  mitto_session_destroy(g_session);
  return result;
}
