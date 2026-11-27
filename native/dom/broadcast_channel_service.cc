#include "mitto/dom/broadcast_channel_service.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "mitto/dom/broadcast_channel.h"
#include "mitto/dom/event_loop.h"
#include "mitto/dom/execution_context.h"
#include "mitto/dom/serialized_script_value.h"
#include "url/origin.h"

namespace mitto {
namespace {

// Where the current thread's channels receive, on TaskType::kInternalDefault,
// as Blink's channel does.
scoped_refptr<base::SingleThreadTaskRunner> CurrentRunner() {
  // The Page program always makes one before it runs a script.
  EventLoop* const event_loop = EventLoop::Get();
  CHECK(event_loop);
  return event_loop->TaskRunner(TaskType::kInternalDefault);
}

}  // namespace

BroadcastChannelService::Connection::Connection(
    uint64_t id,
    std::string key,
    bool opaque,
    bool isolated_by_document_policy,
    scoped_refptr<base::SingleThreadTaskRunner> runner,
    std::string name,
    BroadcastChannel* channel)
    : id(id),
      key(std::move(key)),
      opaque(opaque),
      isolated_by_document_policy(isolated_by_document_policy),
      runner(std::move(runner)),
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

void BroadcastChannelService::SetSessionSender(SessionSender sender) {
  const base::AutoLock lock(lock_);
  session_sender_ = std::move(sender);
}

uint64_t BroadcastChannelService::Connect(BroadcastChannel* channel,
                                          ExecutionContext& context,
                                          const std::u16string& name) {
  const scoped_refptr<base::SingleThreadTaskRunner> runner = CurrentRunner();
  const base::AutoLock lock(lock_);
  // Dispose(): the connections of this thread's channels that the collector
  // took. Another thread's channels are that thread's to look at.
  std::erase_if(connections_, [&runner](const Connection& c) {
    return c.runner == runner && !c.channel;
  });
  const uint64_t id = ++next_connection_;
  // Mojo carries the name as UTF-8, with a lone surrogate as U+FFFD.
  connections_.emplace_back(id, context.StorageKey(),
                            context.GetSecurityOrigin().opaque(),
                            context.IsolatedByDocumentPolicy(), runner,
                            base::UTF16ToUTF8(name), channel);
  return id;
}

void BroadcastChannelService::Disconnect(uint64_t connection) {
  const base::AutoLock lock(lock_);
  std::erase_if(connections_, [connection](const Connection& c) {
    return c.id == connection;
  });
}

void BroadcastChannelService::Post(uint64_t connection,
                                   uint64_t context_id,
                                   const SerializedScriptValue& message) {
  const bool locked = message.IsLockedToAgentCluster();
  // The connections that the message finds once it reaches the service, which
  // are those that connected before it was posted, and whether each is of the
  // sender's agent cluster: of one key, which holds the origin, and of the
  // same isolation by Document-Isolation-Policy.
  struct Recipient {
    uint64_t id;
    scoped_refptr<base::SingleThreadTaskRunner> runner;
    bool same_agent_cluster;
  };
  std::vector<Recipient> recipients;
  {
    const base::AutoLock lock(lock_);
    const auto from = std::find_if(
        connections_.begin(), connections_.end(),
        [connection](const Connection& c) { return c.id == connection; });
    if (from == connections_.end()) {
      return;
    }
    for (const Connection& c : connections_) {
      if (c.id != from->id && c.key == from->key && c.name == from->name) {
        recipients.push_back({c.id, c.runner,
                              c.isolated_by_document_policy ==
                                  from->isolated_by_document_policy});
      }
    }
    if (session_sender_ && !from->opaque) {
      session_sender_.Run(context_id, from->name, locked, message.wire_data());
    }
  }
  // The hop to the service and the one back, each a task of the recipient's
  // thread, so that a sender that closes in the same task loses none, as a
  // message that has gone into Blink's pipe stays there.
  for (Recipient& recipient : recipients) {
    recipient.runner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](BroadcastChannelService* service, uint64_t recipient,
               scoped_refptr<base::SingleThreadTaskRunner> runner, bool locked,
               bool same_agent_cluster, std::vector<uint8_t> data,
               BlobDataHandleMap blob_data_handles) {
              service->Deliver(recipient, std::move(runner), locked,
                               same_agent_cluster, std::move(data),
                               std::move(blob_data_handles));
            },
            base::Unretained(this), recipient.id, recipient.runner, locked,
            recipient.same_agent_cluster, message.wire_data(),
            message.blob_data_handles()));
  }
}

void BroadcastChannelService::OnSessionMessage(const std::string& storage_key,
                                               const std::string& name,
                                               bool locked,
                                               std::vector<uint8_t> data) {
  std::vector<std::pair<uint64_t, scoped_refptr<base::SingleThreadTaskRunner>>>
      recipients;
  {
    const base::AutoLock lock(lock_);
    for (const Connection& c : connections_) {
      if (!c.opaque && c.key == storage_key && c.name == name) {
        recipients.emplace_back(c.id, c.runner);
      }
    }
  }
  for (auto& [recipient, runner] : recipients) {
    // Chrome's browser would hand over the blobs too: a Known Gap.
    Deliver(recipient, std::move(runner), locked, /*same_agent_cluster=*/false,
            data, BlobDataHandleMap());
  }
}

void BroadcastChannelService::Deliver(
    uint64_t connection,
    scoped_refptr<base::SingleThreadTaskRunner> runner,
    bool locked,
    bool same_agent_cluster,
    std::vector<uint8_t> data,
    BlobDataHandleMap blob_data_handles) {
  runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](BroadcastChannelService* service, uint64_t connection, bool locked,
             bool same_agent_cluster, std::vector<uint8_t> data,
             BlobDataHandleMap blob_data_handles) {
            cppgc::subtle::WeakCrossThreadPersistent<BroadcastChannel> handle;
            {
              const base::AutoLock lock(service->lock_);
              const auto found = std::find_if(
                  service->connections_.begin(), service->connections_.end(),
                  [connection](const Connection& c) {
                    return c.id == connection;
                  });
              if (found == service->connections_.end()) {
                return;
              }
              handle = found->channel;
            }
            // This thread's channel, which only it reads.
            BroadcastChannel* const channel = handle.Get();
            if (!channel) {
              return;
            }
            channel->DeliverInContext(
                SerializedScriptValue::FromWireData(
                    std::move(data), std::move(blob_data_handles)),
                locked, same_agent_cluster);
          },
          base::Unretained(this), connection, locked, same_agent_cluster,
          std::move(data), std::move(blob_data_handles)));
}

}  // namespace mitto
