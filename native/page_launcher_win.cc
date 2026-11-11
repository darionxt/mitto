#include <windows.h>

#include <stdint.h>

#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "base/win/scoped_handle.h"
#include "mitto/page_channel.h"
#include "mitto/page_launcher.h"
#include "mitto/page_messages.h"

namespace mitto {
namespace {

// What the library ends a Page process with. TerminateProcess() takes any code,
// and this one says that the library ended it.
constexpr int kKilledExitCode = 1;

// How long a Page process that was killed may take to end, before this gives up
// on reaping it.
constexpr base::TimeDelta kKilledExitTimeout = base::Seconds(5);

std::string DescribeExitCode(int exit_code) {
  const DWORD code = static_cast<DWORD>(exit_code);
  if (code == kPageExitOutOfMemory) {
    return "ran out of memory";
  }
  // An NTSTATUS of an error, such as 0xC0000005 for an access violation.
  if (code >= 0xC0000000) {
    return base::StringPrintf("ended with exception 0x%08lX", code);
  }
  return base::StrCat({"exited with code ", base::NumberToString(code)});
}

}  // namespace

base::FilePath PageProgramPath() {
  static const base::NoDestructor<base::FilePath> path([] {
    // The module of //base's code, which the library links.
    base::FilePath library;
    if (!base::PathService::Get(base::FILE_MODULE, &library)) {
      return base::FilePath();
    }
    return library.DirName().Append(FILE_PATH_LITERAL("mitto_page.exe"));
  }());
  return *path;
}

// static
scoped_refptr<PageProcess> PageProcess::Launch(const base::FilePath& program,
                                               PageChannelEnd theirs) {
  // Ends the process once the last handle to it closes, and keeps a crash from
  // waiting for a dialog of Windows Error Reporting.
  base::win::ScopedHandle job(CreateJobObject(nullptr, nullptr));
  if (!job.is_valid() ||
      !base::SetJobObjectLimitFlags(
          job.get(), JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                         JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION)) {
    return nullptr;
  }
  base::CommandLine command_line(program);
  command_line.AppendArg(base::StrCat(
      {kSessionArgumentPrefix,
       base::NumberToString(HandleToULong(theirs.get())), ",",
       base::NumberToString(base::GetCurrentProcId())}));
  base::LaunchOptions options;
  // Only this handle, and no other that the process holds.
  options.handles_to_inherit.push_back(theirs.get());
  options.job_handle = job.get();
  // The program is a console program, which Windows would give a window of its
  // own when the process that holds the library has no console.
  options.start_hidden = true;
  // Chromium links its programs to run with CET's shadow stacks where the
  // processor has them, and V8's deoptimization swaps stacks in a way that
  // they refuse. So, as Chrome starts its renderers, a Page runs without them.
  // Upstream: content/browser/renderer_host/renderer_sandboxed_process_launcher_delegate.cc
  options.disable_cetcompat = true;
  base::Process process = base::LaunchProcess(command_line, options);
  if (!process.IsValid()) {
    return nullptr;
  }
  scoped_refptr<PageProcess> page_process =
      base::WrapRefCounted(new PageProcess(std::move(process)));
  page_process->job_ = std::move(job);
  return page_process;
}

void PageProcess::Kill() {
  // A handle keeps the process's ID from going to another, so this is safe at
  // any time.
  process_.Terminate(kKilledExitCode, /*wait=*/false);
}

std::string PageProcess::Reap(base::TimeDelta grace) {
  int exit_code = 0;
  if (process_.WaitForExitWithTimeout(grace, &exit_code)) {
    return DescribeExitCode(exit_code);
  }
  Kill();
  if (process_.WaitForExitWithTimeout(kKilledExitTimeout, &exit_code)) {
    return base::StrCat(
        {"did not end in time and ", DescribeExitCode(exit_code)});
  }
  return "did not end, even when killed";
}

}  // namespace mitto
