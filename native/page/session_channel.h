// The Page program's side of its channel to its Session, when the library has
// started it for a Page.

#ifndef MITTO_PAGE_SESSION_CHANNEL_H_
#define MITTO_PAGE_SESSION_CHANNEL_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/process/process_handle.h"
#include "base/types/expected.h"
#include "mitto/page_channel.h"

namespace v8 {
class Isolate;
}  // namespace v8

namespace mitto {

struct PageStart;

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

// Starts V8 for the Page once its start has come, since V8's flags and the
// runtime-enabled features of its isolate come from there, and returns the
// isolate, which this thread has entered, or why the Page refuses the start.
using StartV8Callback =
    base::OnceCallback<base::expected<v8::Isolate*, std::string>(
        const PageStart& start)>;

// Serves the Session at the other end of the channel: reads its messages on a
// thread of its own, starts V8 through `start_v8` when the start comes, and
// runs the Session's scripts in that isolate on this thread, whose task
// executor is the Page's event loop. The process ends when the channel closes,
// when the process that holds the library ends, and when it refuses a
// message, so this returns only if it cannot start.
void ServeSession(SessionArgument session, StartV8Callback start_v8);

}  // namespace mitto

#endif  // MITTO_PAGE_SESSION_CHANNEL_H_
