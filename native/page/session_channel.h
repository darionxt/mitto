// The Page program's side of its channel to its Session, when the library has
// started it for a Page.

#ifndef MITTO_PAGE_SESSION_CHANNEL_H_
#define MITTO_PAGE_SESSION_CHANNEL_H_

#include <optional>
#include <string_view>

#include "base/memory/scoped_refptr.h"
#include "base/process/process_handle.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/bindings/runtime_feature_set.h"
#include "mitto/page/page_v8.h"
#include "mitto/page_channel.h"

namespace mitto {

// What the library starts the Page program with.
struct SessionArgument {
  SessionArgument();
  SessionArgument(SessionArgument&&);
  SessionArgument& operator=(SessionArgument&&);
  ~SessionArgument();

  // The end of the channel that the Page program inherited.
  PageChannelEnd channel;
  // The process that holds the library.
  base::ProcessId parent = 0;
};

// Returns what `argument`, which starts with kSessionArgumentPrefix, says, or
// nullopt if it does not name an end of a channel and a process.
std::optional<SessionArgument> ParseSessionArgument(std::string_view argument);

// Serves the Session at the other end of the channel: reads its messages on a
// thread of its own and runs its scripts on this thread, whose task runner is
// `main_thread`, the Page's event loop. V8 starts on this thread once the
// Session's start message has come, with `options` and the start's Machine
// memory, and with `features`. The process ends when the channel closes, when
// the process that holds the library ends, and when it refuses a message, so
// this returns only if it cannot start.
void ServeSession(SessionArgument session,
                  scoped_refptr<base::SingleThreadTaskRunner> main_thread,
                  PageV8Options options,
                  bindings::RuntimeFeatureSet features);

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_CHANNEL_H_
