// The library's side of a Page: its process and its end of the channel, on the
// network thread, and the events that wait for whichever thread holds the
// Page's handle.

#ifndef MITTO_PAGE_HOST_H_
#define MITTO_PAGE_HOST_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/containers/circular_deque.h"
#include "base/memory/raw_ref.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "mitto/page_channel.h"
#include "mitto/page_launcher.h"
#include "mitto/page_messages.h"

namespace mitto {

class ReadyRequests;

// Returns what mitto_page_open() reads from `json`, or nullopt if it holds an
// unknown key or an invalid value.
std::optional<PageStart> ParsePageOptions(std::string_view json);

// How long a Page process has to end once its channel has closed, before it is
// killed. It ends at once when it reads the end of the stream, whatever its
// scripts are doing.
inline constexpr base::TimeDelta kPageExitGrace = base::Seconds(2);

// How many console messages and uncaught errors of a Page wait untaken at most,
// and how many bytes of their text, so that a Page that writes without end
// cannot fill the memory of the process that holds the library. V8's inspector
// keeps as many console messages for DevTools.
inline constexpr size_t kMaxWaitingReports = 1000;
inline constexpr size_t kMaxWaitingReportBytes = 32 * 1024 * 1024;

// One event of a Page. Each MITTO_PAGE_EVENT type uses the fields that
// MittoPageEvent says it does.
struct PageEvent {
  PageEvent();
  PageEvent(PageEvent&&);
  PageEvent& operator=(PageEvent&&);
  ~PageEvent();

  int32_t type = 0;
  int32_t error = 0;
  uint64_t call = 0;
  std::string json;
  std::string message;
  int32_t level = 0;
};

// Carries a Page's events from the network thread to whichever thread holds the
// Page's handle. It outlives the Page's Session, so that a call made after the
// Session has gone still gets its event.
class PageEvents : public base::RefCountedThreadSafe<PageEvents> {
 public:
  PageEvents();

  PageEvents(const PageEvents&) = delete;
  PageEvents& operator=(const PageEvents&) = delete;

  // Called on any thread. Returns the number of a new call.
  uint64_t NewCall();

  // Called on any thread.
  //
  // Takes the next event into `event` and returns net::OK. Waits for one for up
  // to `timeout`, or for as long as it takes if that is nullopt. Returns
  // net::ERR_IO_PENDING if none arrived in that time, and at once after
  // EndWaits().
  int NextEvent(std::optional<base::TimeDelta> timeout, PageEvent& event);

  // Called on any thread. Ends each wait of NextEvent(), and makes each later
  // one return at once.
  void EndWaits();

  // The rest is called on the network thread.

  void Push(PageEvent event);

  // Pushes a console message or an uncaught error, unless kMaxWaitingReports of
  // them, or kMaxWaitingReportBytes of their text, wait untaken. Then it drops
  // it, and the next one that has room comes after a console warning that says
  // how many were dropped. Returns how many events it pushed.
  int PushReport(PageEvent event);

  // Pushes the MITTO_PAGE_EVENT_ENDED event, and remembers why, for the calls
  // that come later.
  void PushEnded(int32_t error, std::string message);

  // Pushes the failure of `call`, which came after the Page ended.
  void PushCallAfterEnd(uint64_t call);

 private:
  friend class base::RefCountedThreadSafe<PageEvents>;

  ~PageEvents();

  void AddReport(PageEvent event) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  base::Lock lock_;
  base::ConditionVariable event_waiting_{&lock_};
  base::circular_deque<PageEvent> events_ GUARDED_BY(lock_);
  uint64_t next_call_ GUARDED_BY(lock_) = 1;
  // Why the Page ended, once it has.
  int32_t ended_error_ GUARDED_BY(lock_) = 0;
  std::string ended_message_ GUARDED_BY(lock_);
  bool waits_ended_ GUARDED_BY(lock_) = false;
  // The console messages and uncaught errors among `events_`, and their text.
  size_t waiting_reports_ GUARDED_BY(lock_) = 0;
  size_t waiting_report_bytes_ GUARDED_BY(lock_) = 0;
  // How many were dropped since the last one that had room.
  uint64_t dropped_reports_ GUARDED_BY(lock_) = 0;
};

// One Page, on the network thread, where its Session owns it until the Page
// closes or the Session goes. Its events go into its PageEvents, and each lists
// the Page's handle in the Session's ReadyRequests, beside its requests.
class PageHost final : public PageChannel::Delegate {
 public:
  PageHost(scoped_refptr<PageEvents> events,
           scoped_refptr<PageProcess> process,
           ReadyRequests& ready,
           uintptr_t handle);

  PageHost(const PageHost&) = delete;
  PageHost& operator=(const PageHost&) = delete;

  // Closes the channel, which ends the Page process. Whoever closes the Page
  // reaps it.
  ~PageHost() override;

  // Starts to read `ours`, and tells the Page how to start. Returns false if it
  // cannot read it.
  bool Start(PageChannelEnd ours, const PageStart& start);

  void Evaluate(uint64_t call, std::string script);

  // Ends each call in progress and the Page, because its Session is being
  // destroyed, and closes the channel.
  void EndWithSession();

  const scoped_refptr<PageProcess>& process() const { return process_; }

  base::WeakPtr<PageHost> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  // PageChannel::Delegate:
  void OnFrame(uint32_t type, std::string payload) override;
  void OnChannelClosed(PageChannel::Closed closed, std::string why) override;

  // Refuses what the Page sent: tells it why, closes the channel and kills the
  // process, because a Page that sends what it should not may be hostile.
  void Refuse(std::string why);

  // Closes the channel, and finds out how the process ended, without blocking
  // the network thread.
  void WaitForExit();
  void OnExited(std::string how);

  // Fails each call in progress, and then pushes MITTO_PAGE_EVENT_ENDED.
  void End(int32_t error, std::string message);

  void Push(PageEvent event);
  void PushReport(PageEvent event);

  const scoped_refptr<PageEvents> events_;
  const scoped_refptr<PageProcess> process_;
  const raw_ref<ReadyRequests> ready_;
  const uintptr_t handle_;
  std::unique_ptr<PageChannel> channel_;
  bool hello_received_ = false;
  // Why the Page refused what the library sent, if it did.
  std::optional<std::string> refused_by_page_;
  // Why the library refused what the Page sent, if it did.
  std::optional<std::string> refused_by_library_;
  bool waiting_for_exit_ = false;
  bool ended_ = false;
  // The calls in progress, which the Page answers in any order.
  std::set<uint64_t> calls_;
  base::WeakPtrFactory<PageHost> weak_factory_{this};
};

}  // namespace mitto

#endif  // MITTO_PAGE_HOST_H_
