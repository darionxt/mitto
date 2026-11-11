#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <algorithm>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "mitto/page_channel.h"
#include "mitto/page_launcher.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// The Page program finds its end of the channel at this descriptor, the first
// after standard input, output and error.
constexpr int kChildChannelDescriptor = 3;

// How long a Page process that was killed may take to end, before this gives up
// on reaping it.
constexpr base::TimeDelta kKilledExitTimeout = base::Seconds(5);

constexpr base::TimeDelta kFirstPoll = base::Milliseconds(1);
constexpr base::TimeDelta kLongestPoll = base::Milliseconds(50);

std::string DescribeWaitStatus(int status) {
  if (WIFSIGNALED(status)) {
    return base::StrCat(
        {"was ended by signal ", base::NumberToString(WTERMSIG(status))});
  }
  const int code = WEXITSTATUS(status);
  if (code == kPageExitOutOfMemory) {
    return "ran out of memory";
  }
  return base::StrCat({"exited with code ", base::NumberToString(code)});
}

// Lies in the library, so that dladdr() names the library and not the program
// that loaded it.
void AddressInTheLibrary() {}

}  // namespace

base::FilePath PageProgramPath() {
  static const base::NoDestructor<base::FilePath> path([] {
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(&AddressInTheLibrary), &info) ==
            0 ||
        !info.dli_fname) {
      return base::FilePath();
    }
    // The name the library was loaded by, which may be a link, or relative to
    // a directory that the process has left since.
    const base::FilePath library =
        base::MakeAbsoluteFilePath(base::FilePath(info.dli_fname));
    if (library.empty()) {
      return base::FilePath();
    }
    return library.DirName().Append("mitto_page");
  }());
  return *path;
}

// static
scoped_refptr<PageProcess> PageProcess::Launch(const base::FilePath& program,
                                               PageChannelEnd theirs) {
  base::CommandLine command_line(program);
  command_line.AppendArg(base::StrCat(
      {kSessionArgumentPrefix, base::NumberToString(kChildChannelDescriptor),
       ",", base::NumberToString(base::GetCurrentProcId())}));
  base::LaunchOptions options;
  // Every other descriptor closes in the Page process, but standard output and
  // error, which it shares with the process that holds the library.
  options.fds_to_remap.emplace_back(theirs.get(), kChildChannelDescriptor);
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  options.kill_on_parent_death = true;
  // //base does not move a descriptor that already has its number, which
  // would leave it closing when the program starts.
  options.fds_to_remove_cloexec.push_back(theirs.get());
#endif
  base::Process process = base::LaunchProcess(command_line, options);
  if (!process.IsValid()) {
    return nullptr;
  }
  return base::WrapRefCounted(new PageProcess(std::move(process)));
}

void PageProcess::Kill() {
  base::AutoLock lock(lock_);
  if (!gone_) {
    kill(pid_, SIGKILL);
  }
}

std::string PageProcess::Reap(base::TimeDelta grace) {
  bool killed = false;
  base::TimeTicks deadline = base::TimeTicks::Now() + grace;
  base::TimeDelta poll = kFirstPoll;
  while (true) {
    int status = 0;
    pid_t result;
    int error;
    {
      // Under the lock, so that Kill() never signals a process that this has
      // reaped, whose ID may be another's by then.
      base::AutoLock lock(lock_);
      result = HANDLE_EINTR(waitpid(pid_, &status, WNOHANG));
      error = errno;
      if (result == pid_ || (result < 0 && error == ECHILD)) {
        gone_ = true;
      }
    }
    if (result == pid_) {
      const std::string described = DescribeWaitStatus(status);
      return killed ? base::StrCat({"did not end in time and ", described})
                    : described;
    }
    if (result < 0) {
      // A caller that waits for any child, or ignores SIGCHLD, takes it first.
      return error == ECHILD
                 ? "ended, and something else in this process took its exit "
                   "status"
                 : base::StrCat({"could not be waited for: errno ",
                                 base::NumberToString(error)});
    }
    if (base::TimeTicks::Now() >= deadline) {
      if (killed) {
        return "did not end, even when killed";
      }
      Kill();
      killed = true;
      deadline = base::TimeTicks::Now() + kKilledExitTimeout;
    }
    base::PlatformThread::Sleep(poll);
    poll = std::min(poll * 2, kLongestPoll);
  }
}

}  // namespace mitto
