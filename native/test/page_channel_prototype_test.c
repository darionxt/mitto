// Drives the channel prototype through the C API, as Python would, on macOS
// and Linux: Pages start, answer, take a body with flow control, refuse what
// they do not know, and their death reaches the Session's event descriptor.
// Nothing of the prototype merges.
//
// Usage: page_channel_prototype_test [page program]
//
// The Page program defaults to mitto_page_prototype beside this program.

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mitto/mitto.h"

#define STRINGIFY_VALUE(value) #value
#define STRINGIFY(value) STRINGIFY_VALUE(value)

#define ERR_IO_PENDING (-1)
#define TIMEOUT_MS 10000

static const char kSettings[] =
    "{\"version\": " STRINGIFY(MITTO_SETTINGS_VERSION) "}";

static MittoSession* g_session;
static int g_fd = -1;
static int g_failures;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const char* event_name(int32_t type) {
  switch (type) {
    case MITTO_PAGE_EVENT_READY:
      return "ready";
    case MITTO_PAGE_EVENT_RESULT:
      return "result";
    case MITTO_PAGE_EVENT_BODY_DONE:
      return "body done";
    case MITTO_PAGE_EVENT_REFUSED:
      return "refused";
    case MITTO_PAGE_EVENT_GONE:
      return "gone";
    default:
      return "?";
  }
}

// Waits on the Session's descriptor, as asyncio would, until `page` has an
// event, and takes it. Returns 0 if none came in time.
static int32_t next_event(MittoPage* page, char** text) {
  const double deadline = now_ms() + TIMEOUT_MS;
  while (1) {
    int32_t type = 0;
    if (mitto_page_next_event(page, &type, text) == 0) {
      return type;
    }
    struct pollfd watched = {g_fd, POLLIN, 0};
    const int left = (int)(deadline - now_ms());
    if (left <= 0 || poll(&watched, 1, left) <= 0) {
      *text = NULL;
      return 0;
    }
    MittoRequest* ready = NULL;
    while (mitto_session_next_ready_request(g_session, &ready) == 0) {
      if ((void*)ready != (void*)page) {
        fprintf(stderr, "  another handle was listed: %p\n", (void*)ready);
      }
    }
  }
}

static void expect(int ok, const char* what) {
  printf("  %s: %s\n", ok ? "ok" : "FAILED", what);
  if (!ok) {
    ++g_failures;
  }
}

static int32_t expect_event(MittoPage* page, int32_t wanted, char** text) {
  const int32_t type = next_event(page, text);
  printf("  event %s: %s\n", event_name(type), *text ? *text : "(none)");
  if (type != wanted) {
    printf("  FAILED: wanted %s\n", event_name(wanted));
    ++g_failures;
  }
  return type;
}

static void skip_to_gone(MittoPage* page, char** text) {
  while (1) {
    const int32_t type = next_event(page, text);
    printf("  event %s: %s\n", event_name(type), *text ? *text : "(none)");
    if (type == MITTO_PAGE_EVENT_GONE || type == 0) {
      expect(type == MITTO_PAGE_EVENT_GONE, "the Page is gone");
      return;
    }
    mitto_page_text_free(*text);
  }
}

static MittoPage* open_page(const char* program) {
  MittoPage* page = NULL;
  const int32_t error = mitto_page_open(g_session, program, &page);
  if (error != 0) {
    printf("  FAILED: mitto_page_open: %" PRId32 "\n", error);
    ++g_failures;
    return NULL;
  }
  printf("  started Page process %" PRId64 "\n", mitto_page_pid(page));
  return page;
}

static int process_exists(int64_t pid) {
  return kill((pid_t)pid, 0) == 0 || errno != ESRCH;
}

static void wait_for_ready(MittoPage* page) {
  char* text = NULL;
  expect_event(page, MITTO_PAGE_EVENT_READY, &text);
  mitto_page_text_free(text);
}

int main(int argc, char** argv) {
  char program[PATH_MAX];
  if (argc > 1) {
    snprintf(program, sizeof(program), "%s", argv[1]);
  } else {
    snprintf(program, sizeof(program), "%s", argv[0]);
    char* slash = strrchr(program, '/');
    snprintf(slash ? slash + 1 : program,
             sizeof(program) - (size_t)(slash ? slash + 1 - program : 0),
             "mitto_page_prototype");
  }
  if (mitto_session_create(kSettings, strlen(kSettings), &g_session) != 0 ||
      mitto_session_event_fd(g_session, &g_fd) != 0) {
    fprintf(stderr, "no Session\n");
    return 1;
  }
  char* text = NULL;

  printf("A Page answers a script, and its events reach the descriptor\n");
  MittoPage* page = open_page(program);
  if (!page) {
    return 1;
  }
  wait_for_ready(page);
  mitto_page_evaluate(page, "1 + 1", 5);
  expect_event(page, MITTO_PAGE_EVENT_RESULT, &text);
  mitto_page_text_free(text);

  printf("Round trips\n");
  const int round_trips = 2000;
  const double start = now_ms();
  for (int i = 0; i < round_trips; ++i) {
    mitto_page_evaluate(page, "x", 1);
    if (next_event(page, &text) != MITTO_PAGE_EVENT_RESULT) {
      expect(0, "a round trip");
      break;
    }
    mitto_page_text_free(text);
  }
  printf("  %d round trips through the descriptor, %.1f us each\n",
         round_trips, (now_ms() - start) * 1000.0 / round_trips);

  printf("A body streams with flow control\n");
  const double body_start = now_ms();
  mitto_page_stream_body(page, 4 * 1024 * 1024);
  expect_event(page, MITTO_PAGE_EVENT_BODY_DONE, &text);
  expect(text && strstr(text, "held at most") &&
             strtoull(strstr(text, "held at most") + 13, NULL, 10) <= 65536,
         "the Page never held more than its window");
  expect(text && strstr(text, "waited for credit 0 times") == NULL,
         "the library waited for credit");
  printf("  4 MiB in %.0f ms\n", now_ms() - body_start);
  mitto_page_text_free(text);

  printf("A Page that is killed is noticed\n");
  const int64_t pid = mitto_page_pid(page);
  const double killed = now_ms();
  kill((pid_t)pid, SIGKILL);
  expect_event(page, MITTO_PAGE_EVENT_GONE, &text);
  printf("  noticed after %.1f ms\n", now_ms() - killed);
  expect(text && strstr(text, "signal 9") != NULL, "it says signal 9");
  mitto_page_text_free(text);
  mitto_page_close(page);

  printf("A Page that crashes is noticed\n");
  page = open_page(program);
  wait_for_ready(page);
  mitto_page_evaluate(page, "crash", 5);
  expect_event(page, MITTO_PAGE_EVENT_GONE, &text);
  mitto_page_text_free(text);
  mitto_page_close(page);

  printf("The Page refuses a message it does not know\n");
  page = open_page(program);
  wait_for_ready(page);
  mitto_page_send_unknown(page);
  expect_event(page, MITTO_PAGE_EVENT_REFUSED, &text);
  mitto_page_text_free(text);
  skip_to_gone(page, &text);
  mitto_page_text_free(text);
  mitto_page_close(page);

  printf("The library refuses a message it does not know\n");
  page = open_page(program);
  wait_for_ready(page);
  mitto_page_evaluate(page, "send-unknown", 12);
  expect_event(page, MITTO_PAGE_EVENT_REFUSED, &text);
  mitto_page_text_free(text);
  skip_to_gone(page, &text);
  mitto_page_text_free(text);
  mitto_page_close(page);

  printf("A Page of another protocol version is refused\n");
  setenv("MITTO_PAGE_PROTOTYPE_VERSION", "99", 1);
  page = open_page(program);
  unsetenv("MITTO_PAGE_PROTOTYPE_VERSION");
  expect_event(page, MITTO_PAGE_EVENT_REFUSED, &text);
  mitto_page_text_free(text);
  skip_to_gone(page, &text);
  mitto_page_text_free(text);
  mitto_page_close(page);

  printf("Closing a Page ends its process\n");
  page = open_page(program);
  wait_for_ready(page);
  const int64_t closed_pid = mitto_page_pid(page);
  const double closed = now_ms();
  mitto_page_close(page);
  while (process_exists(closed_pid) && now_ms() - closed < TIMEOUT_MS) {
    usleep(1000);
  }
  expect(!process_exists(closed_pid), "the process is gone");
  printf("  gone after %.1f ms\n", now_ms() - closed);

  printf("The Session goes on\n");
  page = open_page(program);
  wait_for_ready(page);
  mitto_page_evaluate(page, "still here", 10);
  expect_event(page, MITTO_PAGE_EVENT_RESULT, &text);
  mitto_page_text_free(text);
  mitto_page_close(page);

  mitto_session_destroy(g_session);
  printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
  return g_failures ? 1 : 0;
}
