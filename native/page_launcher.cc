#include "mitto/page_launcher.h"

#include <string>
#include <utility>

#include "base/process/process.h"
#include "base/synchronization/lock.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"

namespace mitto {

PageProcess::PageProcess(base::Process process)
    : pid_(process.Pid())
#if BUILDFLAG(IS_WIN)
      ,
      process_(std::move(process))
#endif
{
}

PageProcess::~PageProcess() = default;

std::string PageProcess::WaitForExit(base::TimeDelta grace) {
  {
    base::AutoLock lock(lock_);
    if (reaping_) {
      while (!reaped_) {
        reaped_changed_.Wait();
      }
      return exit_;
    }
    reaping_ = true;
  }
  std::string exit;
  {
    const base::ScopedBlockingCall blocking(FROM_HERE,
                                            base::BlockingType::MAY_BLOCK);
    exit = Reap(grace);
  }
  base::AutoLock lock(lock_);
  exit_ = std::move(exit);
  reaped_ = true;
  reaped_changed_.Broadcast();
  return exit_;
}

}  // namespace mitto
