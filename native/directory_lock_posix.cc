#include "mitto/directory_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>

#include <utility>

#include "base/check.h"
#include "base/posix/eintr_wrapper.h"

namespace mitto {

DirectoryLock::DirectoryLock() = default;
DirectoryLock::DirectoryLock(DirectoryLock&&) = default;
DirectoryLock& DirectoryLock::operator=(DirectoryLock&&) = default;
DirectoryLock::~DirectoryLock() = default;

// flock() goes by open file, so it refuses a lock from the same process too.
DirectoryLock::Result DirectoryLock::Take(const base::FilePath& directory) {
  DCHECK(!directory_.is_valid());
  base::ScopedFD file(
      HANDLE_EINTR(open(directory.value().c_str(), O_RDONLY | O_CLOEXEC)));
  if (!file.is_valid()) {
    return Result::kFailed;
  }
  if (HANDLE_EINTR(flock(file.get(), LOCK_EX | LOCK_NB)) != 0) {
    return errno == EWOULDBLOCK ? Result::kHeld : Result::kFailed;
  }
  directory_ = std::move(file);
  return Result::kTaken;
}

}  // namespace mitto
