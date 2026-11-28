// The BroadcastChannels of the Page, as the browser's BroadcastChannelService
// keeps a connection for each: by storage key and name, in the order they
// connected. A message goes to every other connection of its key and name that
// is there when it reaches the service, and to the Session, which hands it to
// its other Pages of the same storage key, as Chrome's browser process hands
// it to the renderers of other tabs.
//
// It stands where the browser does: a message reaches it from a task of its
// own, and each connection gets it from another, as a mojo message each way
// would, on the thread of the connection's channel, the main thread's or a
// dedicated worker's. Its state is held under a lock. A message carries its
// bytes alone, as a CloneableMessage does through the browser, so a WebAssembly
// module in it reads back as nothing.
//
// Upstream: content/browser/broadcast_channel/broadcast_channel_service.cc
// Upstream: third_party/blink/renderer/modules/broadcastchannel/broadcast_channel.cc

#ifndef MITTO_DOM_BROADCAST_CHANNEL_SERVICE_H_
#define MITTO_DOM_BROADCAST_CHANNEL_SERVICE_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "mitto/dom/serialized_script_value.h"
#include "v8/include/cppgc/cross-thread-persistent.h"

namespace mitto {

class BroadcastChannel;
class ExecutionContext;

class BroadcastChannelService {
 public:
  // Sends a message of a channel of `name` of the context numbered
  // `context_id` to the Session: whether it is locked to the Page's agent
  // cluster, and its bytes.
  using SessionSender =
      base::RepeatingCallback<void(uint64_t context_id,
                                   const std::string& name,
                                   bool locked,
                                   std::vector<uint8_t> data)>;

  // The Page's.
  static BroadcastChannelService& Get();

  BroadcastChannelService();
  BroadcastChannelService(const BroadcastChannelService&) = delete;
  BroadcastChannelService& operator=(const BroadcastChannelService&) = delete;
  ~BroadcastChannelService();

  // What the Page program gives it once it has a Session, which any thread
  // calls. Without one, as in the Page program given a script, messages stay
  // in the Page.
  void SetSessionSender(SessionSender sender);

  // Connects `channel` of `context` under `name`, and returns the ID of its
  // connection. On the channel's thread.
  uint64_t Connect(BroadcastChannel* channel,
                   ExecutionContext& context,
                   const std::u16string& name);
  void Disconnect(uint64_t connection);

  // A message of the channel of `connection`, whose context is numbered
  // `context_id`.
  void Post(uint64_t connection,
            uint64_t context_id,
            const SerializedScriptValue& message);

  // A message from another Page of the Session, for the channels of
  // `storage_key`, which the Session matched with one of this Page's contexts.
  void OnSessionMessage(const std::string& storage_key,
                        const std::string& name,
                        bool locked,
                        std::vector<uint8_t> data);

 private:
  struct Connection {
    Connection(uint64_t id,
               std::string key,
               bool opaque,
               bool isolated_by_document_policy,
               scoped_refptr<base::SingleThreadTaskRunner> runner,
               std::string name,
               BroadcastChannel* channel);
    Connection(Connection&&);
    Connection& operator=(Connection&&);
    ~Connection();

    uint64_t id;
    // The storage key of the channel's context, and whether its origin is
    // opaque, whose messages stay in the Page.
    std::string key;
    bool opaque;
    // Whether Document-Isolation-Policy isolates the channel's agent cluster,
    // which Chrome has in a process of its own, so that a value locked to it
    // is an error in a channel of the same key without it.
    bool isolated_by_document_policy;
    // Where the channel receives, on its thread.
    scoped_refptr<base::SingleThreadTaskRunner> runner;
    // In UTF-8, as the name reaches Chrome's browser.
    std::string name;
    // Read only on the channel's thread.
    cppgc::subtle::WeakCrossThreadPersistent<BroadcastChannel> channel;
  };

  // Gives the message to the channel of `connection` from a task of its own,
  // on its thread, if it is still connected then, with the blobs it carries,
  // which are those of a message of this Page.
  void Deliver(uint64_t connection,
               scoped_refptr<base::SingleThreadTaskRunner> runner,
               bool locked,
               bool same_agent_cluster,
               std::vector<uint8_t> data,
               BlobDataHandleMap blob_data_handles);

  base::Lock lock_;
  std::vector<Connection> connections_ GUARDED_BY(lock_);
  uint64_t next_connection_ GUARDED_BY(lock_) = 0;
  SessionSender session_sender_ GUARDED_BY(lock_);
};

}  // namespace mitto

#endif  // MITTO_DOM_BROADCAST_CHANNEL_SERVICE_H_
