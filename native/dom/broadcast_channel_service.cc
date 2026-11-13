#include "mitto/dom/broadcast_channel_service.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/broadcast_channel.h"
#include "mitto/dom/document.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/serialized_script_value.h"
#include "mitto/dom/window.h"
#include "mitto/dom/window_task.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mitto {
namespace {

void PostServiceTask(base::OnceClosure task) {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  event_loop->TaskRunner(EventLoop::Queue::kDefault)
      ->PostTask(FROM_HERE, std::move(task));
}

}  // namespace

BroadcastChannelService::Connection::Connection(uint64_t id,
                                                std::string key,
                                                bool opaque,
                                                Window* window,
                                                std::string name,
                                                BroadcastChannel* channel)
    : id(id),
      key(std::move(key)),
      opaque(opaque),
      window(window),
      name(std::move(name)),
      channel(channel) {}
BroadcastChannelService::Connection::Connection(Connection&&) = default;
BroadcastChannelService::Connection&
BroadcastChannelService::Connection::operator=(Connection&&) = default;
BroadcastChannelService::Connection::~Connection() = default;

// static
BroadcastChannelService& BroadcastChannelService::Get() {
  static base::NoDestructor<BroadcastChannelService> service;
  return *service;
}

BroadcastChannelService::BroadcastChannelService() = default;
BroadcastChannelService::~BroadcastChannelService() = default;

uint64_t BroadcastChannelService::Connect(BroadcastChannel* channel,
                                          Window& window,
                                          const std::u16string& name) {
  const url::Origin origin = url::Origin::Create(
      GURL(base::UTF16ToUTF8(window.GetDocument().UrlForBinding())));
  // Dispose(): the connections of channels that the collector took.
  std::erase_if(connections_, [](const Connection& c) { return !c.channel; });
  const uint64_t id = ++next_connection_;
  // Mojo carries the name as UTF-8, with a lone surrogate as U+FFFD.
  connections_.emplace_back(id, origin.Serialize(), origin.opaque(), &window,
                            base::UTF16ToUTF8(name), channel);
  return id;
}

void BroadcastChannelService::Disconnect(uint64_t connection) {
  std::erase_if(connections_, [connection](const Connection& c) {
    return c.id == connection;
  });
}

void BroadcastChannelService::Post(uint64_t connection,
                                   const SerializedScriptValue& message) {
  const auto from = std::find_if(
      connections_.begin(), connections_.end(),
      [connection](const Connection& c) { return c.id == connection; });
  if (from == connections_.end()) {
    return;
  }
  const bool locked = message.IsLockedToAgentCluster();
  // The connections that the message finds once it reaches the service, which
  // are those that connected before it was posted.
  std::vector<uint64_t> recipients;
  for (const Connection& c : connections_) {
    if (c.id != from->id && c.channel && c.key == from->key &&
        c.name == from->name &&
        (!from->opaque || c.window.Get() == from->window.Get())) {
      recipients.push_back(c.id);
    }
  }
  if (session_sender_ && !from->opaque) {
    session_sender_.Run(from->name, locked, message.wire_data());
  }
  PostServiceTask(base::BindOnce(
      [](BroadcastChannelService* service, std::vector<uint64_t> recipients,
         bool locked, std::vector<uint8_t> data) {
        for (const uint64_t recipient : recipients) {
          service->Deliver(recipient, locked, /*same_agent_cluster=*/true,
                           data);
        }
      },
      base::Unretained(this), std::move(recipients), locked,
      message.wire_data()));
}

void BroadcastChannelService::OnSessionMessage(const std::string& name,
                                               bool locked,
                                               std::vector<uint8_t> data) {
  for (const Connection& c : connections_) {
    if (c.channel && !c.opaque && c.name == name) {
      Deliver(c.id, locked, /*same_agent_cluster=*/false, data);
    }
  }
}

void BroadcastChannelService::Deliver(uint64_t connection,
                                      bool locked,
                                      bool same_agent_cluster,
                                      std::vector<uint8_t> data) {
  PostServiceTask(base::BindOnce(
      [](BroadcastChannelService* service, uint64_t connection, bool locked,
         bool same_agent_cluster, std::vector<uint8_t> data) {
        const auto found = std::find_if(
            service->connections_.begin(), service->connections_.end(),
            [connection](const Connection& c) { return c.id == connection; });
        if (found == service->connections_.end()) {
          return;
        }
        BroadcastChannel* const channel = found->channel.Get();
        if (!channel) {
          return;
        }
        RunInWindowContext(found->window.Get(), [&] {
          channel->OnMessage(
              SerializedScriptValue::FromWireData(std::move(data)), locked,
              same_agent_cluster);
        });
      },
      base::Unretained(this), connection, locked, same_agent_cluster,
      std::move(data)));
}

}  // namespace mitto
