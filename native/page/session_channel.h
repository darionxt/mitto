// The Page program's side of its channel to its Session, when the library has
// started it for a Page.

#ifndef MITTO_PAGE_SESSION_CHANNEL_H_
#define MITTO_PAGE_SESSION_CHANNEL_H_

#include <optional>
#include <string_view>

#include "base/process/process_handle.h"
#include "mitto/page_channel.h"

namespace v8 {
class Isolate;
class Platform;
}  // namespace v8

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
// thread of its own and runs its scripts in `isolate` on this thread. The
// process ends when the channel closes, when the process that holds the library
// ends, and when it refuses a message, so this returns only if it cannot start.
void ServeSession(v8::Platform& platform,
                  v8::Isolate* isolate,
                  SessionArgument session);

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_CHANNEL_H_
