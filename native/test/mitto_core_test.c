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

#if !defined(_WIN32)
#include <poll.h>
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
    MittoRequest* ready;
    while (mitto_session_next_ready_request(g_session, &ready) == 0) {
      if (ready != g_request) {
        fprintf(stderr, "another request is ready\n");
        return 1;
      }
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
