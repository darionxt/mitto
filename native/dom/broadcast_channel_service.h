// The BroadcastChannels of the Page, as the browser's BroadcastChannelService
// keeps a connection for each: by storage key and name, in the order they
// connected. A message goes to every other connection of its key and name that
// is there when it reaches the service, and to the Session, which hands it to
// its other Pages of the same storage key, as Chrome's browser process hands
// it to the renderers of other tabs.
//
// It stands where the browser does: a message reaches it from a task of its
// own, and each connection gets it from another, as a mojo message each way
// would. Until frames have storage keys of their own (#261), a channel's key
// is its document's origin, since a Page has only its top-level document, and
// channels of an opaque origin reach only those of their own window. A message
// carries its bytes alone, as a CloneableMessage does through the browser, so
// a WebAssembly module in it reads back as nothing.
//
// Upstream: content/browser/broadcast_channel/broadcast_channel_service.cc
// Upstream: third_party/blink/renderer/modules/broadcastchannel/broadcast_channel.cc

#ifndef MITTO_DOM_BROADCAST_CHANNEL_SERVICE_H_
#define MITTO_DOM_BROADCAST_CHANNEL_SERVICE_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "v8/include/cppgc/persistent.h"

namespace mitto {

class BroadcastChannel;
class SerializedScriptValue;
class Window;

class BroadcastChannelService {
 public:
  // Sends a message of a channel of `name` to the Session: whether it is
  // locked to the Page's agent cluster, and its bytes.
  using SessionSender = base::RepeatingCallback<
      void(const std::string& name, bool locked, std::vector<uint8_t> data)>;

  // The Page's.
  static BroadcastChannelService& Get();

  BroadcastChannelService();
  BroadcastChannelService(const BroadcastChannelService&) = delete;
  BroadcastChannelService& operator=(const BroadcastChannelService&) = delete;
  ~BroadcastChannelService();

  // What the Page program gives it once it has a Session. Without one, as in
  // the Page program given a script, messages stay in the Page.
  void SetSessionSender(SessionSender sender) {
    session_sender_ = std::move(sender);
  }

  // Connects `channel` of `window` under `name`, and returns the ID of its
  // connection.
  uint64_t Connect(BroadcastChannel* channel,
                   Window& window,
                   const std::u16string& name);
  void Disconnect(uint64_t connection);

  // A message of the channel of `connection`.
  void Post(uint64_t connection, const SerializedScriptValue& message);

  // A message from another Page of the Session, whose document's origin the
  // Session matched with this Page's.
  void OnSessionMessage(const std::string& name,
                        bool locked,
                        std::vector<uint8_t> data);

 private:
  struct Connection {
    Connection(uint64_t id,
               std::string key,
               bool opaque,
               Window* window,
               std::string name,
               BroadcastChannel* channel);
    Connection(Connection&&);
    Connection& operator=(Connection&&);
    ~Connection();

    uint64_t id;
    // The serialized origin of the channel's document.
    std::string key;
    bool opaque;
    cppgc::WeakPersistent<Window> window;
    // In UTF-8, as the name reaches Chrome's browser.
    std::string name;
    cppgc::WeakPersistent<BroadcastChannel> channel;
  };

  // Gives the message to the channel of `connection` from a task of its own,
  // if it is still connected then.
  void Deliver(uint64_t connection,
               bool locked,
               bool same_agent_cluster,
               std::vector<uint8_t> data);

  std::vector<Connection> connections_;
  uint64_t next_connection_ = 0;
  SessionSender session_sender_;
};

}  // namespace mitto

#endif  // MITTO_DOM_BROADCAST_CHANNEL_SERVICE_H_
