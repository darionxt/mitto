#ifndef MITTO_DIRECTORY_LOCK_H_
#define MITTO_DIRECTORY_LOCK_H_

#include "base/files/file_path.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/scoped_handle.h"
#else
#include <sys/types.h>

#include "base/files/scoped_file.h"
#endif

namespace mitto {

// A lock on a directory. No other lock can take the same directory while this
// one holds it: not from another process, not from this one, and not through
// another name for the directory, such as a link. It goes away with this
// object, or with the process, however that ends, and no child process ever
// holds it, however it starts.
class DirectoryLock {
 public:
  // What Take() did.
  enum class Result {
    kTaken,
    // Another lock holds the directory.
    kHeld,
    // The directory cannot be opened, or on POSIX its lock file.
    kFailed,
  };

  // A lock that holds nothing yet.
  DirectoryLock();

  DirectoryLock(DirectoryLock&&);
  DirectoryLock& operator=(DirectoryLock&&);

  ~DirectoryLock();

  // Takes `directory`, which must exist. Called once at most. On POSIX it makes
  // the file kFileName in the directory, if it is not there, and leaves it
  // there.
  Result Take(const base::FilePath& directory);

#if !BUILDFLAG(IS_WIN)
  // The name of the lock file. No Chromium component writes it at the top of
  // a directory of the HTTP cache or of storage: //net writes in `Cache_Data`,
  // and LevelDB, which writes `LOCK` at the top of each database, would let
  // this process's lock go if it opened and closed the same file.
  static constexpr char kFileName[] = "mitto.lock";
#endif

 private:
#if BUILDFLAG(IS_WIN)
  // A mutex whose name says which directory it is for.
  base::win::ScopedHandle mutex_;
#else
  // Lets the lock go, if it holds one.
  void Release();

  // The lock file, open, with fcntl()'s lock on it.
  base::ScopedFD file_;
  // The directory's device and inode, by which this process knows what its
  // locks hold.
  dev_t device_ = 0;
  ino_t inode_ = 0;
#endif
};

}  // namespace mitto

#endif  // MITTO_DIRECTORY_LOCK_H_
