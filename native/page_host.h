// The library's side of the channel prototype: starts a Page program and talks
// to it on the network thread. Nothing of it merges.

#ifndef MITTO_PAGE_HOST_H_
#define MITTO_PAGE_HOST_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/containers/circular_deque.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ref.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "mitto/page_channel.h"

namespace mitto {

class ReadyRequests;

struct PageEvent {
  int32_t type = 0;
  std::string text;
};

// Carries a Page's events from the network thread to whichever thread holds
// the Page's handle.
class PageEvents : public base::RefCountedThreadSafe<PageEvents> {
 public:
  PageEvents();

  void Add(PageEvent event);
  std::optional<PageEvent> Take();

 private:
  friend class base::RefCountedThreadSafe<PageEvents>;
  ~PageEvents();

  base::Lock lock_;
  base::circular_deque<PageEvent> events_ GUARDED_BY(lock_);
};

// One Page process, on the network thread. Its events go into `events`, and
// each lists `handle` in the Session's ReadyRequests, beside its requests.
class PageHost final : public ChannelEnd::Delegate {
 public:
  PageHost(scoped_refptr<PageEvents> events,
           ReadyRequests& ready,
           uintptr_t handle);

  PageHost(const PageHost&) = delete;
  PageHost& operator=(const PageHost&) = delete;

  // Closes the channel, which ends the Page process, and reaps it.
  ~PageHost() override;

  // Starts the program and says hello. Returns the process ID, or nullopt.
  std::optional<int64_t> Launch(const base::FilePath& program);

  void Evaluate(std::string script);
  void StreamBody(uint64_t size);
  void SendUnknownForTest();

  base::WeakPtr<PageHost> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  // ChannelEnd::Delegate:
  void OnFrame(uint32_t type, std::string payload) override;
  void OnChannelClosed(std::string_view why) override;

  void Emit(int32_t type, std::string text);
  void Refuse(std::string why);
  void PumpBody();
  void WatchForExit(std::string why);
  void OnExited(std::string why, std::string status);

  scoped_refptr<PageEvents> events_;
  const raw_ref<ReadyRequests> ready_;
  const uintptr_t handle_;
  base::Process process_;
  std::unique_ptr<ChannelEnd> channel_;
  bool hello_received_ = false;
  // A task reaps the process, or will.
  bool reaping_ = false;

  // The body on its way to the Page.
  uint64_t body_left_ = 0;
  uint64_t body_credit_ = 0;
  bool body_open_ = false;
  // How often the body waited for the Page to give credit back.
  int body_stalls_ = 0;

  base::WeakPtrFactory<PageHost> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_HOST_H_
