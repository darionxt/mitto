#include "mitto/directory_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <utility>

#include "base/check.h"
#include "base/containers/flat_set.h"
#include "base/no_destructor.h"
#include "base/posix/eintr_wrapper.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace mitto {
namespace {

// fcntl()'s lock belongs to the process, as base::File::Lock() says: another
// process cannot take it, and a child never holds it, as it would hold a
// flock() on a descriptor it got a copy of while it started, which
// posix_spawn() gives it of every descriptor, close-on-exec ones too, until its
// program has loaded. But a second lock of this process takes it again rather
// than being refused, and closing any descriptor of the file lets it go. So
// this process keeps the directories that its locks hold, by device and inode,
// which are the same by any name, and opens a lock file only for one that none
// holds.
struct HeldDirectories {
  base::Lock lock;
  base::flat_set<std::pair<dev_t, ino_t>> held GUARDED_BY(lock);
};

HeldDirectories& GetHeldDirectories() {
  static base::NoDestructor<HeldDirectories> held_directories;
  return *held_directories;
}

}  // namespace

DirectoryLock::DirectoryLock() = default;

DirectoryLock::DirectoryLock(DirectoryLock&& other)
    : file_(std::move(other.file_)),
      device_(other.device_),
      inode_(other.inode_) {}

DirectoryLock& DirectoryLock::operator=(DirectoryLock&& other) {
  if (this != &other) {
    Release();
    file_ = std::move(other.file_);
    device_ = other.device_;
    inode_ = other.inode_;
  }
  return *this;
}

DirectoryLock::~DirectoryLock() {
  Release();
}

DirectoryLock::Result DirectoryLock::Take(const base::FilePath& directory) {
  DCHECK(!file_.is_valid());
  HeldDirectories& held_directories = GetHeldDirectories();
  const base::AutoLock auto_lock(held_directories.lock);
  struct stat info;
  if (stat(directory.value().c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
    return Result::kFailed;
  }
  if (held_directories.held.contains({info.st_dev, info.st_ino})) {
    return Result::kHeld;
  }
  base::ScopedFD file(
      HANDLE_EINTR(open(directory.Append(kFileName).value().c_str(),
                        O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR)));
  if (!file.is_valid()) {
    return Result::kFailed;
  }
  struct flock lock = {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  // The whole file.
  lock.l_len = 0;
  if (HANDLE_EINTR(fcntl(file.get(), F_SETLK, &lock)) != 0) {
    // Closing the file lets go of no lock of this process, which holds none on
    // it.
    return errno == EACCES || errno == EAGAIN ? Result::kHeld : Result::kFailed;
  }
  held_directories.held.insert({info.st_dev, info.st_ino});
  file_ = std::move(file);
  device_ = info.st_dev;
  inode_ = info.st_ino;
  return Result::kTaken;
}

void DirectoryLock::Release() {
  if (!file_.is_valid()) {
    return;
  }
  HeldDirectories& held_directories = GetHeldDirectories();
  const base::AutoLock auto_lock(held_directories.lock);
  file_.reset();
  held_directories.held.erase({device_, inode_});
}

}  // namespace mitto
