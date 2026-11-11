// Starting the Page program from the library, and ending and reaping its
// process, on each Host.

#ifndef MITTO_PAGE_LAUNCHER_H_
#define MITTO_PAGE_LAUNCHER_H_

#include <string>

#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "mitto/page_channel.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/scoped_handle.h"
#endif

namespace mitto {

// Returns the Page program in the directory that the library was loaded from,
// which is where a wheel puts it. It may not exist, such as in a wheel that a
// release made.
base::FilePath PageProgramPath();

// The process of a Page program. On Windows it runs in a job object of its
// own, which ends it once the last handle to the job closes, as when the
// process that holds the library ends.
class PageProcess : public base::RefCountedThreadSafe<PageProcess> {
 public:
  // Starts `program` with `theirs` as its end of the channel. Call this on a
  // thread that lives as long as the process: on Linux the kernel kills the
  // Page process when the thread that started it ends, which stands for the
  // end of the process that holds the library. Returns null if it cannot.
  static scoped_refptr<PageProcess> Launch(const base::FilePath& program,
                                           PageChannelEnd theirs);

  PageProcess(const PageProcess&) = delete;
  PageProcess& operator=(const PageProcess&) = delete;

  base::ProcessId pid() const { return pid_; }

  // Ends the process at once, unless it has been reaped. Called on any thread.
  void Kill();

  // Waits for the process to end, for up to `grace` before it kills it, reaps
  // it, and returns how it ended, such as "was ended by signal 9". Only the
  // first call reaps the process: a later or concurrent one waits for that one,
  // and returns the same. Blocks, so it runs on a thread that may, such as the
  // caller's or one of the thread pool's, and not on the network thread.
  std::string WaitForExit(base::TimeDelta grace);

 private:
  friend class base::RefCountedThreadSafe<PageProcess>;

  explicit PageProcess(base::Process process);
  ~PageProcess();

  // What WaitForExit() does for the first call.
  std::string Reap(base::TimeDelta grace);

  const base::ProcessId pid_;
  base::Lock lock_;
  base::ConditionVariable reaped_changed_{&lock_};
  bool reaping_ GUARDED_BY(lock_) = false;
  bool reaped_ GUARDED_BY(lock_) = false;
  std::string exit_ GUARDED_BY(lock_);
#if BUILDFLAG(IS_WIN)
  base::Process process_;
  base::win::ScopedHandle job_;
#else
  // The process has been reaped, by this or by something else in the process
  // that holds the library, so its ID may belong to another process now.
  bool gone_ GUARDED_BY(lock_) = false;
#endif
};

}  // namespace mitto

#endif  // MITTO_PAGE_LAUNCHER_H_
